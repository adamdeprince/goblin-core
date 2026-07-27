// Cross-host latency qualification for Goblin's kernel TCP and libfabric RDM
// transports. RESP2 and SBE run the same depth-one operations and validate every
// reply. Pub/Sub measures publish request through subscriber delivery.

#include "goblin/core/libfabric_transport.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kClientBufferBytes = 128U * 1024U;
constexpr std::string_view kValueA = "value-0000000001";
constexpr std::string_view kValueB = "value-0000000002";
constexpr std::string_view kPubSubPayload = "quote-update";

double nanoseconds_per_tick = 1.0;

[[gnu::always_inline]] inline std::uint64_t hardware_ticks() noexcept {
#if defined(__x86_64__)
  unsigned auxiliary = 0;
  return __rdtscp(&auxiliary);
#elif defined(__aarch64__)
  std::uint64_t ticks = 0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#else
  return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

void calibrate_ticks() {
#if defined(__aarch64__)
  std::uint64_t frequency = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  nanoseconds_per_tick = 1.0e9 / static_cast<double>(frequency);
#elif defined(__x86_64__)
  const auto start_time = Clock::now();
  const auto start_ticks = hardware_ticks();
  while (Clock::now() - start_time < 250ms) {
  }
  const auto finish_ticks = hardware_ticks();
  const auto finish_time = Clock::now();
  nanoseconds_per_tick =
      std::chrono::duration<double, std::nano>(finish_time - start_time).count() /
      static_cast<double>(finish_ticks - start_ticks);
#endif
}

template <class Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] double percentile_us(const std::vector<std::uint64_t>& values,
                                   double fraction) {
  const auto rank = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(values.size())));
  const std::size_t index = std::min(values.size() - 1,
                                     rank == 0 ? std::size_t{0} : rank - 1);
  return static_cast<double>(values[index]) * nanoseconds_per_tick / 1000.0;
}

template <class Operation>
[[nodiscard]] bool measure(std::string_view label, std::string_view operation,
                           std::size_t warmup, std::size_t samples,
                           Operation&& invoke) {
  for (std::size_t index = 0; index < warmup; ++index) {
    if (!invoke()) {
      std::fprintf(stderr, "%.*s: %.*s failed during warmup\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data());
      return false;
    }
  }

  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(samples);
  for (std::size_t index = 0; index < samples; ++index) {
    const auto begin = hardware_ticks();
    const bool ok = invoke();
    const auto finish = hardware_ticks();
    if (!ok) {
      std::fprintf(stderr, "%.*s: %.*s failed at sample %zu\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data(), index);
      return false;
    }
    elapsed.push_back(finish - begin);
  }

  std::ranges::sort(elapsed);
  const double total_ticks = std::accumulate(
      elapsed.begin(), elapsed.end(), 0.0,
      [](double total, std::uint64_t value) { return total + value; });
  const double mean_us = total_ticks * nanoseconds_per_tick /
                         static_cast<double>(elapsed.size()) / 1000.0;
  const double qps = 1.0e6 / mean_us;

  std::printf(
      "LAT,%.*s,%.*s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%zu\n",
      static_cast<int>(label.size()), label.data(),
      static_cast<int>(operation.size()), operation.data(),
      percentile_us(elapsed, 0.0), percentile_us(elapsed, 0.50),
      percentile_us(elapsed, 0.75), percentile_us(elapsed, 0.90),
      percentile_us(elapsed, 0.95), percentile_us(elapsed, 0.99),
      percentile_us(elapsed, 0.999), percentile_us(elapsed, 0.9999),
      percentile_us(elapsed, 1.0), mean_us, qps, elapsed.size());
  std::fflush(stdout);
  return true;
}

class TcpRespClient {
 public:
  TcpRespClient(const TcpRespClient&) = delete;
  TcpRespClient& operator=(const TcpRespClient&) = delete;
  TcpRespClient(TcpRespClient&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        pending_(std::move(other.pending_)),
        send_buffer_bytes_(other.send_buffer_bytes_),
        receive_buffer_bytes_(other.receive_buffer_bytes_) {}
  ~TcpRespClient() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  [[nodiscard]] static std::optional<TcpRespClient> open(
      std::string_view host, std::uint16_t port,
      std::string_view local_address, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    const std::string host_text(host);
    const std::string port_text = std::to_string(port);
    const int lookup = ::getaddrinfo(host_text.c_str(), port_text.c_str(),
                                     &hints, &addresses);
    if (lookup != 0) {
      error = std::string("getaddrinfo: ") + ::gai_strerror(lookup);
      return std::nullopt;
    }

    int fd = -1;
    int last_error = 0;
    for (addrinfo* remote = addresses; remote != nullptr;
         remote = remote->ai_next) {
      fd = ::socket(remote->ai_family, remote->ai_socktype,
                    remote->ai_protocol);
      if (fd < 0) {
        last_error = errno;
        continue;
      }
      const int enabled = 1;
      (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                         sizeof(enabled));
      timeval timeout{.tv_sec = 5, .tv_usec = 0};
      (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
      (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));

      bool bound = true;
      if (!local_address.empty()) {
        addrinfo local_hints{};
        local_hints.ai_family = remote->ai_family;
        local_hints.ai_socktype = SOCK_STREAM;
        local_hints.ai_protocol = IPPROTO_TCP;
        local_hints.ai_flags = AI_NUMERICHOST;
        addrinfo* locals = nullptr;
        const std::string local_text(local_address);
        const int local_lookup = ::getaddrinfo(
            local_text.c_str(), "0", &local_hints, &locals);
        if (local_lookup != 0 || locals == nullptr ||
            ::bind(fd, locals->ai_addr,
                   static_cast<socklen_t>(locals->ai_addrlen)) != 0) {
          bound = false;
          last_error = errno;
        }
        if (locals != nullptr) {
          ::freeaddrinfo(locals);
        }
      }
      if (bound &&
          ::connect(fd, remote->ai_addr,
                    static_cast<socklen_t>(remote->ai_addrlen)) == 0) {
        break;
      }
      last_error = errno;
      (void)::close(fd);
      fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) {
      error = std::string("connect: ") + std::strerror(last_error);
      return std::nullopt;
    }
    return TcpRespClient(fd);
  }

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> arguments) {
    const std::string encoded = goblin::core::ring::encode_command(arguments);
    if (!send_bytes(encoded)) {
      return std::nullopt;
    }
    return read_reply();
  }

  [[nodiscard]] std::optional<std::string> read_reply() {
    for (;;) {
      if (const auto end = goblin::core::ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      char buffer[16 * 1024];
      const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
      if (received > 0) {
        pending_.append(buffer, static_cast<std::size_t>(received));
      } else if (received < 0 && errno == EINTR) {
        continue;
      } else {
        return std::nullopt;
      }
    }
  }

  [[nodiscard]] int send_buffer_bytes() const noexcept {
    return send_buffer_bytes_;
  }
  [[nodiscard]] int receive_buffer_bytes() const noexcept {
    return receive_buffer_bytes_;
  }

 private:
  explicit TcpRespClient(int fd) : fd_(fd) {
    socklen_t length = sizeof(send_buffer_bytes_);
    (void)::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes_,
                       &length);
    length = sizeof(receive_buffer_bytes_);
    (void)::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes_,
                       &length);
    pending_.reserve(kClientBufferBytes);
  }

  [[nodiscard]] bool send_bytes(std::string_view bytes) {
    while (!bytes.empty()) {
      const ssize_t sent =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  int fd_{-1};
  std::string pending_;
  int send_buffer_bytes_{0};
  int receive_buffer_bytes_{0};
};

class FabricRespClient {
 public:
  FabricRespClient(const FabricRespClient&) = delete;
  FabricRespClient& operator=(const FabricRespClient&) = delete;
  FabricRespClient(FabricRespClient&&) noexcept = default;
  FabricRespClient& operator=(FabricRespClient&&) noexcept = default;

  [[nodiscard]] static std::optional<FabricRespClient> open(
      std::string_view provider, std::string_view host, std::uint16_t port,
      std::string_view local_address, std::string& error,
      goblin::core::fabric::SendMode send_mode) {
    auto transport = goblin::core::fabric::ClientTransport::open(
        provider, host, port, 5s, kClientBufferBytes, local_address, &error,
        send_mode);
    if (!transport) {
      return std::nullopt;
    }
    return FabricRespClient(std::move(*transport));
  }

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> arguments) {
    const std::string encoded = goblin::core::ring::encode_command(arguments);
    if (!transport_.send(encoded, [] { return false; })) {
      return std::nullopt;
    }
    return read_reply();
  }

  [[nodiscard]] std::optional<std::string> read_reply() {
    for (;;) {
      if (const auto end = goblin::core::ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      if (transport_.failed()) {
        return std::nullopt;
      }
      if (auto record = transport_.peek()) {
        pending_.append(record->data(), record->size());
        transport_.pop();
      } else {
        transport_.wait_for_record();
      }
    }
  }

 private:
  explicit FabricRespClient(goblin::core::fabric::ClientTransport&& transport)
      : transport_(std::move(transport)) {
    pending_.reserve(kClientBufferBytes);
  }

  goblin::core::fabric::ClientTransport transport_;
  std::string pending_;
};

[[nodiscard]] bool is_reply(const std::optional<std::string>& reply,
                            std::string_view expected) {
  return reply && *reply == expected;
}

template <class Client>
[[nodiscard]] bool run_resp_suite(Client& client, Client& subscriber,
                                  std::string_view label,
                                  std::string_view transport,
                                  std::string_view endpoint,
                                  std::string_view buffers,
                                  std::size_t samples, std::size_t warmup) {
  const std::string key_prefix = "fabric-bench:" + std::string(label);
  const std::string string_key = key_prefix + ":string";
  const std::string hash_key = key_prefix + ":hash";
  const std::string zset_key = key_prefix + ":zset";
  const std::string channel = key_prefix + ":channel";

  const std::array<std::string_view, 3> seed_set{"SET", string_key, kValueA};
  const std::array<std::string_view, 4> seed_hset{
      "HSET", hash_key, "field", kValueA};
  const std::array<std::string_view, 4> seed_zadd{
      "ZADD", zset_key, "1", "member"};
  const std::array<std::string_view, 2> subscribe{"SUBSCRIBE", channel};
  if (!is_reply(client.command(seed_set), "+OK\r\n") ||
      !is_reply(client.command(seed_hset), ":1\r\n") ||
      !is_reply(client.command(seed_zadd), ":1\r\n") ||
      !subscriber.command(subscribe)) {
    std::fprintf(stderr, "%.*s: RESP fixture setup failed\n",
                 static_cast<int>(label.size()), label.data());
    return false;
  }

  const std::array<std::string_view, 1> ping{"PING"};
  std::array<std::string_view, 3> set{"SET", string_key, kValueA};
  const std::array<std::string_view, 2> get{"GET", string_key};
  std::array<std::string_view, 4> hset{
      "HSET", hash_key, "field", kValueA};
  const std::array<std::string_view, 3> hget{"HGET", hash_key, "field"};
  std::array<std::string_view, 4> zadd{"ZADD", zset_key, "1", "member"};
  const std::array<std::string_view, 3> zscore{
      "ZSCORE", zset_key, "member"};
  const std::array<std::string_view, 3> publish{
      "PUBLISH", channel, kPubSubPayload};

  std::printf("META,%.*s,%.*s,RESP2,depth=1,%.*s,%.*s,client-work=%zu,samples=%zu,warmup=%zu\n",
              static_cast<int>(label.size()), label.data(),
              static_cast<int>(transport.size()), transport.data(),
              static_cast<int>(endpoint.size()), endpoint.data(),
              static_cast<int>(buffers.size()), buffers.data(),
              kClientBufferBytes, samples, warmup);
  std::fflush(stdout);

  bool toggle = false;
  return measure(label, "PING", warmup, samples, [&] {
           return is_reply(client.command(ping), "+PONG\r\n");
         }) &&
         measure(label, "SET", warmup, samples, [&] {
           toggle = !toggle;
           set[2] = toggle ? kValueA : kValueB;
           return is_reply(client.command(set), "+OK\r\n");
         }) &&
         measure(label, "GET", warmup, samples, [&] {
           const auto reply = client.command(get);
           return is_reply(reply, "$16\r\nvalue-0000000001\r\n") ||
                  is_reply(reply, "$16\r\nvalue-0000000002\r\n");
         }) &&
         measure(label, "HSET", warmup, samples, [&] {
           toggle = !toggle;
           hset[3] = toggle ? kValueA : kValueB;
           return is_reply(client.command(hset), ":0\r\n");
         }) &&
         measure(label, "HGET", warmup, samples, [&] {
           const auto reply = client.command(hget);
           return is_reply(reply, "$16\r\nvalue-0000000001\r\n") ||
                  is_reply(reply, "$16\r\nvalue-0000000002\r\n");
         }) &&
         measure(label, "ZADD", warmup, samples, [&] {
           toggle = !toggle;
           zadd[2] = toggle ? "1" : "2";
           return is_reply(client.command(zadd), ":0\r\n");
         }) &&
         measure(label, "ZSCORE", warmup, samples, [&] {
           const auto reply = client.command(zscore);
           return is_reply(reply, "$1\r\n1\r\n") ||
                  is_reply(reply, "$1\r\n2\r\n");
         }) &&
         measure(label, "PUBSUB", warmup, samples, [&] {
           if (!is_reply(client.command(publish), ":1\r\n")) {
             return false;
           }
           const auto message = subscriber.read_reply();
           return message &&
                  message->find(channel) != std::string::npos &&
                  message->find(kPubSubPayload) != std::string::npos;
         });
}

template <class Client>
[[nodiscard]] bool run_sbe_suite(Client& client, Client& subscriber,
                                 std::string_view label,
                                 std::string_view transport,
                                 std::string_view endpoint,
                                 std::string_view buffers,
                                 std::size_t samples, std::size_t warmup) {
  const std::string key_prefix = "fabric-bench:" + std::string(label);
  const std::string string_key = key_prefix + ":string";
  const std::string hash_key = key_prefix + ":hash";
  const std::string zset_key = key_prefix + ":zset";
  const std::string channel = key_prefix + ":channel";
  const std::array<std::pair<std::string_view, std::string_view>, 1> seed_hash{{
      {"field", kValueA},
  }};
  const std::array<typename Client::Scored, 1> seed_zset{{
      {1.0, "member"},
  }};
  const std::array<std::string_view, 1> channels{channel};
  if (!client.set(string_key, kValueA).ok ||
      client.hset(hash_key, seed_hash) != 1 ||
      client.zadd(zset_key, seed_zset) != 1 ||
      subscriber.subscribe(channels).size() != 1) {
    std::fprintf(stderr, "%.*s: SBE fixture setup failed\n",
                 static_cast<int>(label.size()), label.data());
    return false;
  }

  std::printf("META,%.*s,%.*s,SBE,depth=1,%.*s,%.*s,client-work=%zu,samples=%zu,warmup=%zu\n",
              static_cast<int>(label.size()), label.data(),
              static_cast<int>(transport.size()), transport.data(),
              static_cast<int>(endpoint.size()), endpoint.data(),
              static_cast<int>(buffers.size()), buffers.data(),
              kClientBufferBytes, samples, warmup);
  std::fflush(stdout);

  bool toggle = false;
  return measure(label, "PING", warmup, samples,
                 [&] { return client.ping(); }) &&
         measure(label, "SET", warmup, samples, [&] {
           toggle = !toggle;
           return client.set(string_key, toggle ? kValueA : kValueB).ok;
         }) &&
         measure(label, "GET", warmup, samples, [&] {
           const auto value = client.get(string_key);
           return value && (*value == kValueA || *value == kValueB);
         }) &&
         measure(label, "HSET", warmup, samples, [&] {
           toggle = !toggle;
           const std::array<std::pair<std::string_view, std::string_view>, 1>
               entry{{{"field", toggle ? kValueA : kValueB}}};
           return client.hset(hash_key, entry) == 0;
         }) &&
         measure(label, "HGET", warmup, samples, [&] {
           const auto value = client.hget(hash_key, "field");
           return value && (*value == kValueA || *value == kValueB);
         }) &&
         measure(label, "ZADD", warmup, samples, [&] {
           toggle = !toggle;
           const std::array<typename Client::Scored, 1> member{{
               {toggle ? 1.0 : 2.0, "member"},
           }};
           return client.zadd(zset_key, member) == 0;
         }) &&
         measure(label, "ZSCORE", warmup, samples, [&] {
           const auto score = client.zscore(zset_key, "member");
           return score && (*score == 1.0 || *score == 2.0);
         }) &&
         measure(label, "PUBSUB", warmup, samples, [&] {
           if (client.publish(channel, kPubSubPayload) != 1) {
             return false;
           }
           const auto message = subscriber.read_pubsub();
           return message.kind == goblin::core::PubSubKind::message &&
                  message.channel == channel &&
                  message.payload == kPubSubPayload;
         });
}

void usage(const char* program) {
  std::fprintf(
      stderr,
      "usage:\n"
      "  %s tcp-resp HOST PORT LOCAL-ADDRESS|- LABEL SAMPLES WARMUP\n"
      "  %s tcp-sbe  HOST PORT LABEL SAMPLES WARMUP\n"
      "  %s fabric-resp PROVIDER HOST PORT LOCAL-ADDRESS|- LABEL SAMPLES WARMUP"
      " [auto|send]\n"
      "  %s fabric-sbe  PROVIDER HOST PORT LOCAL-ADDRESS|- LABEL SAMPLES WARMUP"
      " [auto|send]\n",
      program, program, program, program);
}

[[nodiscard]] bool parse_run_counts(std::string_view port_text,
                                    std::string_view samples_text,
                                    std::string_view warmup_text,
                                    std::uint16_t& port,
                                    std::size_t& samples,
                                    std::size_t& warmup) {
  unsigned port_value = 0;
  if (!parse_integer(port_text, port_value) || port_value == 0 ||
      port_value > 65535 || !parse_integer(samples_text, samples) ||
      samples == 0 || !parse_integer(warmup_text, warmup)) {
    return false;
  }
  port = static_cast<std::uint16_t>(port_value);
  return true;
}

[[nodiscard]] std::optional<goblin::core::fabric::SendMode> parse_send_mode(
    std::string_view text) {
  if (text == "auto") {
    return goblin::core::fabric::SendMode::auto_inject;
  }
  if (text == "send") {
    return goblin::core::fabric::SendMode::send;
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  calibrate_ticks();
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  const std::string_view mode = argv[1];
  std::string error;

  if (mode == "tcp-resp") {
    if (argc != 8) {
      usage(argv[0]);
      return 2;
    }
    std::uint16_t port = 0;
    std::size_t samples = 0;
    std::size_t warmup = 0;
    if (!parse_run_counts(argv[3], argv[6], argv[7], port, samples, warmup)) {
      std::fprintf(stderr, "invalid numeric argument\n");
      return 2;
    }
    const std::string_view local = std::string_view(argv[4]) == "-" ? "" : argv[4];
    auto client = TcpRespClient::open(argv[2], port, local, error);
    auto subscriber = TcpRespClient::open(argv[2], port, local, error);
    if (!client || !subscriber) {
      std::fprintf(stderr, "kernel TCP connect failed: %s\n", error.c_str());
      return 1;
    }
    const std::string buffers =
        "snd=" + std::to_string(client->send_buffer_bytes()) +
        ";rcv=" + std::to_string(client->receive_buffer_bytes());
    const std::string endpoint =
        std::string(local.empty() ? "kernel-route" : local) + "->" + argv[2] +
        ":" + std::to_string(port);
    return run_resp_suite(*client, *subscriber, argv[5], "kernel-tcp",
                          endpoint, buffers, samples, warmup)
               ? 0
               : 1;
  }

  if (mode == "tcp-sbe") {
    if (argc != 7) {
      usage(argv[0]);
      return 2;
    }
    std::uint16_t port = 0;
    std::size_t samples = 0;
    std::size_t warmup = 0;
    if (!parse_run_counts(argv[3], argv[5], argv[6], port, samples, warmup)) {
      std::fprintf(stderr, "invalid numeric argument\n");
      return 2;
    }
    const auto endpoint =
        goblin::core::SbeSocketEndpoint::tcp(argv[2], port);
    auto client = goblin::core::SbeSocketClient::open(
        endpoint, 5s, kClientBufferBytes, &error);
    auto subscriber = goblin::core::SbeSocketClient::open(
        endpoint, 5s, kClientBufferBytes, &error);
    if (!client || !subscriber) {
      std::fprintf(stderr, "SBE TCP connect failed: %s\n", error.c_str());
      return 1;
    }
    const std::string target =
        "kernel-route->" + std::string(argv[2]) + ":" + std::to_string(port);
    return run_sbe_suite(*client, *subscriber, argv[4], "kernel-tcp", target,
                         "client-work=131072;TCP_NODELAY", samples, warmup)
               ? 0
               : 1;
  }

  if (mode != "fabric-resp" && mode != "fabric-sbe") {
    usage(argv[0]);
    return 2;
  }
  if (argc != 9 && argc != 10) {
    usage(argv[0]);
    return 2;
  }

  std::uint16_t port = 0;
  std::size_t samples = 0;
  std::size_t warmup = 0;
  if (!parse_run_counts(argv[4], argv[7], argv[8], port, samples, warmup)) {
    std::fprintf(stderr, "invalid numeric argument\n");
    return 2;
  }
  const auto send_mode = parse_send_mode(argc == 10 ? argv[9] : "auto");
  if (!send_mode) {
    std::fprintf(stderr, "invalid libfabric send mode\n");
    return 2;
  }
  const std::string_view local = std::string_view(argv[5]) == "-" ? "" : argv[5];
  const std::string endpoint =
      std::string(local.empty() ? "provider-route" : local) + "->" + argv[3] +
      ":" + std::to_string(port);
  const std::string buffers =
      "tx-slots=16;rx-slots=16;max-payload=131072;reorder=64/1048576;send=" +
      std::string(*send_mode == goblin::core::fabric::SendMode::send
                      ? "fi_send"
                      : "auto-inject");

  if (mode == "fabric-resp") {
    auto client = FabricRespClient::open(argv[2], argv[3], port, local, error,
                                         *send_mode);
    auto subscriber = FabricRespClient::open(
        argv[2], argv[3], port, local, error, *send_mode);
    if (!client || !subscriber) {
      std::fprintf(stderr, "libfabric RESP connect failed: %s\n",
                   error.c_str());
      return 1;
    }
    const std::string transport = "libfabric-RDM:" + std::string(argv[2]);
    return run_resp_suite(*client, *subscriber, argv[6], transport, endpoint,
                          buffers, samples, warmup)
               ? 0
               : 1;
  }

  auto client = goblin::core::SbeLibfabricClient::open(
      argv[2], argv[3], port, 5s, kClientBufferBytes, local, &error,
      *send_mode);
  auto subscriber = goblin::core::SbeLibfabricClient::open(
      argv[2], argv[3], port, 5s, kClientBufferBytes, local, &error,
      *send_mode);
  if (!client || !subscriber) {
    std::fprintf(stderr, "libfabric SBE connect failed: %s\n", error.c_str());
    return 1;
  }
  const std::string transport = "libfabric-RDM:" + std::string(argv[2]);
  return run_sbe_suite(*client, *subscriber, argv[6], transport, endpoint,
                       buffers, samples, warmup)
             ? 0
             : 1;
}
