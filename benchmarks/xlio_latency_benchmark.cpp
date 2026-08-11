// Depth-one RESP latency over Goblin's shared-memory ring, kernel TCP, and
// native XLIO Ultra TCP transports. Every mode runs the same commands and
// validates every reply, so cross-mode results isolate the transport path.

#include "goblin/core/ring_client.hpp"
#if defined(GOBLIN_HAS_XLIO)
#include "goblin/core/xlio_client.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
  while (Clock::now() - start_time < std::chrono::milliseconds(250)) {
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
  for (std::size_t i = 0; i < warmup; ++i) {
    if (!invoke()) {
      std::fprintf(stderr, "%.*s: %.*s failed during warmup\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data());
      return false;
    }
  }

  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto begin = hardware_ticks();
    const bool ok = invoke();
    const auto end = hardware_ticks();
    if (!ok) {
      std::fprintf(stderr, "%.*s: %.*s failed at sample %zu\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data(), i);
      return false;
    }
    elapsed.push_back(end - begin);
  }

  std::ranges::sort(elapsed);
  const double total_ticks = std::accumulate(
      elapsed.begin(), elapsed.end(), 0.0,
      [](double total, std::uint64_t value) { return total + value; });
  const double mean_us = total_ticks * nanoseconds_per_tick /
                         static_cast<double>(elapsed.size()) / 1000.0;
  const double qps = 1.0e6 / mean_us;
  const double minimum = percentile_us(elapsed, 0.0);
  const double p50 = percentile_us(elapsed, 0.50);
  const double p75 = percentile_us(elapsed, 0.75);
  const double p90 = percentile_us(elapsed, 0.90);
  const double p95 = percentile_us(elapsed, 0.95);
  const double p99 = percentile_us(elapsed, 0.99);
  const double p999 = percentile_us(elapsed, 0.999);
  const double p9999 = percentile_us(elapsed, 0.9999);
  const double maximum = percentile_us(elapsed, 1.0);

  std::printf(
      "LAT,%.*s,%.*s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%zu\n",
      static_cast<int>(label.size()), label.data(),
      static_cast<int>(operation.size()), operation.data(), minimum, p50, p75,
      p90, p95, p99, p999, p9999, maximum, mean_us, qps, elapsed.size());
  std::fflush(stdout);
  std::fprintf(stderr,
               "  %-7.*s p50=%8.3f p99=%8.3f p99.9=%8.3f "
               "p99.99=%8.3f max=%9.3f us\n",
               static_cast<int>(operation.size()), operation.data(), p50, p99,
               p999, p9999, maximum);
  return true;
}

template <class Operation>
[[nodiscard]] bool measure_external_ticks(std::string_view label,
                                          std::string_view operation,
                                          std::size_t warmup,
                                          std::size_t samples,
                                          Operation&& invoke) {
  for (std::size_t i = 0; i < warmup; ++i) {
    if (!invoke()) {
      std::fprintf(stderr, "%.*s: %.*s failed during warmup\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data());
      return false;
    }
  }

  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto ticks = invoke();
    if (!ticks) {
      std::fprintf(stderr, "%.*s: %.*s failed at sample %zu\n",
                   static_cast<int>(label.size()), label.data(),
                   static_cast<int>(operation.size()), operation.data(), i);
      return false;
    }
    elapsed.push_back(*ticks);
  }

  std::ranges::sort(elapsed);
  const double total_ticks = std::accumulate(
      elapsed.begin(), elapsed.end(), 0.0,
      [](double total, std::uint64_t value) { return total + value; });
  const double mean_us = total_ticks * nanoseconds_per_tick /
                         static_cast<double>(elapsed.size()) / 1000.0;
  const double qps = 1.0e6 / mean_us;
  const double minimum = percentile_us(elapsed, 0.0);
  const double p50 = percentile_us(elapsed, 0.50);
  const double p75 = percentile_us(elapsed, 0.75);
  const double p90 = percentile_us(elapsed, 0.90);
  const double p95 = percentile_us(elapsed, 0.95);
  const double p99 = percentile_us(elapsed, 0.99);
  const double p999 = percentile_us(elapsed, 0.999);
  const double p9999 = percentile_us(elapsed, 0.9999);
  const double maximum = percentile_us(elapsed, 1.0);

  std::printf(
      "LAT,%.*s,%.*s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.0f,%zu\n",
      static_cast<int>(label.size()), label.data(),
      static_cast<int>(operation.size()), operation.data(), minimum, p50, p75,
      p90, p95, p99, p999, p9999, maximum, mean_us, qps, elapsed.size());
  std::fflush(stdout);
  std::fprintf(stderr,
               "  %-7.*s p50=%8.3f p99=%8.3f p99.9=%8.3f "
               "p99.99=%8.3f max=%9.3f us\n",
               static_cast<int>(operation.size()), operation.data(), p50, p99,
               p999, p9999, maximum);
  return true;
}

class TcpClient {
 public:
  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;
  TcpClient(TcpClient&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)), pending_(std::move(other.pending_)),
        send_buffer_bytes_(other.send_buffer_bytes_),
        receive_buffer_bytes_(other.receive_buffer_bytes_) {}
  ~TcpClient() {
    if (fd_ >= 0) (void)::close(fd_);
  }

  [[nodiscard]] static std::optional<TcpClient> open(
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
    const int gai = ::getaddrinfo(host_text.c_str(), port_text.c_str(), &hints,
                                  &addresses);
    if (gai != 0) {
      error = std::string("getaddrinfo: ") + ::gai_strerror(gai);
      return std::nullopt;
    }

    int fd = -1;
    for (addrinfo* remote = addresses; remote != nullptr;
         remote = remote->ai_next) {
      fd = ::socket(remote->ai_family, remote->ai_socktype,
                    remote->ai_protocol);
      if (fd < 0) continue;
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
        if (::getaddrinfo(local_text.c_str(), "0", &local_hints, &locals) != 0 ||
            locals == nullptr ||
            ::bind(fd, locals->ai_addr,
                   static_cast<socklen_t>(locals->ai_addrlen)) != 0) {
          bound = false;
        }
        if (locals != nullptr) ::freeaddrinfo(locals);
      }
      if (bound &&
          ::connect(fd, remote->ai_addr,
                    static_cast<socklen_t>(remote->ai_addrlen)) == 0) {
        break;
      }
      (void)::close(fd);
      fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) {
      error = std::string("connect: ") + std::strerror(errno);
      return std::nullopt;
    }
    return TcpClient(fd);
  }

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> arguments) {
    try {
      send(arguments);
    } catch (const std::exception&) {
      return std::nullopt;
    }
    return read_reply();
  }

  void send(std::span<const std::string_view> arguments) {
    send_raw(goblin::core::ring::encode_command(arguments));
  }

  void send_raw(std::string_view encoded) {
    std::size_t offset = 0;
    while (offset < encoded.size()) {
      const ssize_t sent = ::send(fd_, encoded.data() + offset,
                                  encoded.size() - offset, MSG_NOSIGNAL);
      if (sent > 0) {
        offset += static_cast<std::size_t>(sent);
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else {
        throw std::runtime_error("TcpClient: request send failed");
      }
    }
  }

  [[nodiscard]] std::optional<std::string> read_reply() {
    for (;;) {
      if (const auto end = goblin::core::ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      char buffer[8192];
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
  explicit TcpClient(int fd) : fd_(fd) {
    socklen_t size = sizeof(send_buffer_bytes_);
    (void)::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes_, &size);
    size = sizeof(receive_buffer_bytes_);
    (void)::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes_, &size);
    pending_.reserve(8192);
  }

  int fd_{-1};
  std::string pending_;
  int send_buffer_bytes_{0};
  int receive_buffer_bytes_{0};
};

[[nodiscard]] bool status_reply(const std::optional<std::string>& reply,
                                std::string_view expected) {
  return reply && *reply == expected;
}

[[nodiscard]] bool integer_reply(const std::optional<std::string>& reply) {
  return reply && !reply->empty() && reply->front() == ':';
}

[[nodiscard]] bool bulk_reply(const std::optional<std::string>& reply) {
  return reply && !reply->empty() && reply->front() == '$';
}

template <class Client>
[[nodiscard]] bool run_suite(Client& client, std::string_view label,
                             std::string_view transport,
                             std::string_view buffer_description,
                             std::size_t samples, std::size_t warmup) {
  const std::vector<std::string_view> seed_set{"SET", "xlio:lat:string",
                                               "v1"};
  const std::vector<std::string_view> seed_hset{
      "HSET", "xlio:lat:hash", "f0", "v0", "f1", "v1", "f2", "v2",
      "f3", "v3", "f4", "v4", "f5", "v5", "f6", "v6", "f7", "v7",
      "f8", "v8", "f9", "v9"};
  const std::vector<std::string_view> seed_zadd{
      "ZADD", "xlio:lat:zset", "0", "m0", "1", "m1", "2", "m2",
      "3", "m3", "4", "m4", "5", "m5", "6", "m6", "7", "m7",
      "8", "m8", "9", "m9"};
  if (!status_reply(client.command(seed_set), "+OK\r\n") ||
      !integer_reply(client.command(seed_hset)) ||
      !integer_reply(client.command(seed_zadd))) {
    std::fprintf(stderr, "%.*s: fixture setup failed\n",
                 static_cast<int>(label.size()), label.data());
    return false;
  }

  const std::vector<std::string_view> ping{"PING"};
  const std::vector<std::string_view> set{"SET", "xlio:lat:string", "v1"};
  const std::vector<std::string_view> get{"GET", "xlio:lat:string"};
  const std::vector<std::string_view> hset{"HSET", "xlio:lat:hash", "f5",
                                           "v5"};
  const std::vector<std::string_view> hget{"HGET", "xlio:lat:hash", "f5"};
  const std::vector<std::string_view> zadd{"ZADD", "xlio:lat:zset", "5",
                                           "m5"};
  const std::vector<std::string_view> zscore{"ZSCORE", "xlio:lat:zset", "m5"};

  const char* operation_environment =
      std::getenv("GOBLIN_XLIO_BENCH_OPERATION");
  const std::string_view selected_operation =
      operation_environment == nullptr ? std::string_view{}
                                       : std::string_view(operation_environment);
  const auto selected = [&](std::string_view operation) {
    return selected_operation.empty() || selected_operation == operation;
  };
  const bool known_operation =
      selected_operation.empty() || selected_operation == "PING" ||
      selected_operation == "SET" || selected_operation == "GET" ||
      selected_operation == "HSET" || selected_operation == "HGET" ||
      selected_operation == "ZADD" || selected_operation == "ZSCORE";
  if (!known_operation) {
    std::fprintf(stderr, "%.*s: unknown operation filter %.*s\n",
                 static_cast<int>(label.size()), label.data(),
                 static_cast<int>(selected_operation.size()),
                 selected_operation.data());
    return false;
  }

  std::printf("META,%.*s,%.*s,%.*s,%zu,%zu,RESP2,1\n",
              static_cast<int>(label.size()), label.data(),
              static_cast<int>(transport.size()), transport.data(),
              static_cast<int>(buffer_description.size()),
              buffer_description.data(), samples, warmup);
  std::fflush(stdout);
  std::fprintf(stderr,
               "[%.*s] %.*s; %.*s; RESP2 depth=1; samples=%zu warmup=%zu\n",
               static_cast<int>(label.size()), label.data(),
               static_cast<int>(transport.size()), transport.data(),
               static_cast<int>(buffer_description.size()),
               buffer_description.data(), samples, warmup);

  return (!selected("PING") ||
          measure(label, "PING", warmup, samples, [&] {
            return status_reply(client.command(ping), "+PONG\r\n");
          })) &&
         (!selected("SET") ||
          measure(label, "SET", warmup, samples, [&] {
            return status_reply(client.command(set), "+OK\r\n");
          })) &&
         (!selected("GET") ||
          measure(label, "GET", warmup, samples,
                  [&] { return bulk_reply(client.command(get)); })) &&
         (!selected("HSET") ||
          measure(label, "HSET", warmup, samples,
                  [&] { return integer_reply(client.command(hset)); })) &&
         (!selected("HGET") ||
          measure(label, "HGET", warmup, samples,
                  [&] { return bulk_reply(client.command(hget)); })) &&
         (!selected("ZADD") ||
          measure(label, "ZADD", warmup, samples,
                  [&] { return integer_reply(client.command(zadd)); })) &&
         (!selected("ZSCORE") ||
          measure(label, "ZSCORE", warmup, samples,
                  [&] { return bulk_reply(client.command(zscore)); }));
}

[[nodiscard]] std::string pubsub_ack(std::string_view channel) {
  std::string reply("*3\r\n$9\r\nsubscribe\r\n$");
  reply.append(std::to_string(channel.size()));
  reply.append("\r\n");
  reply.append(channel);
  reply.append("\r\n:1\r\n");
  return reply;
}

template <class Client>
[[nodiscard]] bool run_pubsub_suite(Client& subscriber, Client& publisher,
                                    std::string_view label,
                                    std::string_view transport,
                                    std::string_view buffer_description,
                                    std::size_t samples, std::size_t warmup,
                                    std::string_view channel) {
  const std::array<std::string_view, 2> subscribe{"SUBSCRIBE", channel};
  if (subscriber.command(subscribe) != pubsub_ack(channel)) {
    std::fprintf(stderr, "%.*s: SUBSCRIBE setup failed\n",
                 static_cast<int>(label.size()), label.data());
    return false;
  }

  std::printf("META,%.*s,%.*s,%.*s,%zu,%zu,RESP2,1\n",
              static_cast<int>(label.size()), label.data(),
              static_cast<int>(transport.size()), transport.data(),
              static_cast<int>(buffer_description.size()),
              buffer_description.data(), samples, warmup);
  std::fflush(stdout);
  std::fprintf(stderr,
               "[%.*s] %.*s; %.*s; RESP2 local publish-to-subscriber; "
               "samples=%zu warmup=%zu\n",
               static_cast<int>(label.size()), label.data(),
               static_cast<int>(transport.size()), transport.data(),
               static_cast<int>(buffer_description.size()),
               buffer_description.data(), samples, warmup);

  std::uint64_t sequence = 0;
  return measure_external_ticks(label, "PUBSUB", warmup, samples,
                                [&]() -> std::optional<std::uint64_t> {
    const std::string payload = "p" + std::to_string(sequence++);
    const std::array<std::string_view, 3> publish{"PUBLISH", channel, payload};
    const std::array<std::string_view, 3> message{"message", channel, payload};
    try {
      const auto begin = hardware_ticks();
      publisher.send(publish);
      const auto delivered = subscriber.read_reply();
      const auto end = hardware_ticks();
      const auto publish_reply = publisher.read_reply();
      if (!delivered ||
          *delivered != goblin::core::ring::encode_command(message) ||
          publish_reply != std::optional<std::string>(":1\r\n")) {
        return std::nullopt;
      }
      return end - begin;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  });
}

void usage(const char* program) {
  std::fprintf(
      stderr,
      "usage:\n"
      "  %s ring PATH LABEL SAMPLES WARMUP\n"
      "  %s tcp HOST PORT LABEL SAMPLES WARMUP [LOCAL-ADDRESS]\n"
      "  %s xlio HOST PORT LABEL SAMPLES WARMUP [LOCAL-ADDRESS]\n"
      "  %s tcp-pubsub HOST PORT LABEL SAMPLES WARMUP [LOCAL-ADDRESS] [CHANNEL]\n"
      "  %s xlio-pubsub HOST PORT LABEL SAMPLES WARMUP [LOCAL-ADDRESS] [CHANNEL]\n"
      "\nAll modes use RESP2 and pipeline depth 1. Pub/Sub modes time local "
      "PUBLISH-to-subscriber delivery.\n",
      program, program, program, program, program);
  std::fprintf(stderr,
               "Set GOBLIN_XLIO_BENCH_OPERATION to one operation for a "
               "diagnostic-only run.\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  calibrate_ticks();
  const std::string_view mode = argv[1];

  if (mode == "ring") {
    if (argc != 6) {
      usage(argv[0]);
      return 2;
    }
    std::size_t samples = 0;
    std::size_t warmup = 0;
    if (!parse_integer<std::size_t>(argv[4], samples) || samples == 0 ||
        !parse_integer<std::size_t>(argv[5], warmup)) {
      std::fprintf(stderr, "xlio_latency_benchmark: invalid sample count\n");
      return 2;
    }
    auto client = goblin::core::ring::RingClient::open(
        argv[2], std::chrono::seconds(5));
    if (!client) {
      std::fprintf(stderr, "xlio_latency_benchmark: cannot open ring %s\n",
                   argv[2]);
      return 1;
    }
    const std::string buffers =
        "sq=" + std::to_string(client->mapping().sq_capacity()) +
        ";cq=" + std::to_string(client->mapping().cq_capacity());
    return run_suite(*client, argv[3], "shared-memory-ring", buffers, samples,
                     warmup)
               ? 0
               : 1;
  }

  const bool pubsub = mode == "tcp-pubsub" || mode == "xlio-pubsub";
  const bool tcp = mode == "tcp" || mode == "tcp-pubsub";
  const bool xlio = mode == "xlio" || mode == "xlio-pubsub";
  if (!tcp && !xlio) {
    usage(argv[0]);
    return 2;
  }
  if ((!pubsub && argc != 7 && argc != 8) ||
      (pubsub && (argc < 7 || argc > 9))) {
    usage(argv[0]);
    return 2;
  }
  unsigned port_value = 0;
  std::size_t samples = 0;
  std::size_t warmup = 0;
  if (!parse_integer<unsigned>(argv[3], port_value) || port_value == 0 ||
      port_value > 65535 ||
      !parse_integer<std::size_t>(argv[5], samples) || samples == 0 ||
      !parse_integer<std::size_t>(argv[6], warmup)) {
    std::fprintf(stderr, "xlio_latency_benchmark: invalid numeric argument\n");
    return 2;
  }
  const auto port = static_cast<std::uint16_t>(port_value);
  const std::string_view local_address = argc >= 8 ? argv[7] : "";
  const std::string_view channel =
      argc >= 9 ? argv[8] : "benchmark:bluefield:xlio";
  std::string error;

  if (tcp) {
    auto client = TcpClient::open(argv[2], port, local_address, error);
    if (!client) {
      std::fprintf(stderr, "xlio_latency_benchmark: kernel TCP: %s\n",
                   error.c_str());
      return 1;
    }
    const std::string buffers =
        "snd=" + std::to_string(client->send_buffer_bytes()) +
        ";rcv=" + std::to_string(client->receive_buffer_bytes());
    if (pubsub) {
      auto publisher = TcpClient::open(argv[2], port, local_address, error);
      if (!publisher) {
        std::fprintf(stderr, "xlio_latency_benchmark: kernel TCP: %s\n",
                     error.c_str());
        return 1;
      }
      return run_pubsub_suite(*client, *publisher, argv[4], "kernel-tcp",
                              buffers, samples, warmup, channel)
                 ? 0
                 : 1;
    }
    return run_suite(*client, argv[4], "kernel-tcp", buffers, samples, warmup)
               ? 0
               : 1;
  }

#if defined(GOBLIN_HAS_XLIO)
  auto client = goblin::core::xlio::XlioClient::open(
      argv[2], port, std::chrono::seconds(5), local_address, &error);
  if (!client) {
    std::fprintf(stderr, "xlio_latency_benchmark: XLIO Ultra: %s\n",
                 error.c_str());
    return 1;
  }
  const std::string buffers =
      "client-work=" +
      std::to_string(client->transport().buffer_size_hint()) +
      ";tx=inline-copy;rx=xlio-owned;pipeline=1";
  if (pubsub) {
    auto publisher = goblin::core::xlio::XlioClient::open(
        argv[2], port, std::chrono::seconds(5), local_address, &error);
    if (!publisher) {
      std::fprintf(stderr, "xlio_latency_benchmark: XLIO Ultra: %s\n",
                   error.c_str());
      return 1;
    }
    return run_pubsub_suite(*client, *publisher, argv[4], "xlio-ultra-tcp",
                            buffers, samples, warmup, channel)
               ? 0
               : 1;
  }
  return run_suite(*client, argv[4], "xlio-ultra-tcp", buffers, samples,
                   warmup)
             ? 0
             : 1;
#else
  std::fprintf(stderr,
               "xlio_latency_benchmark: xlio mode requires "
               "-DGOBLIN_CORE_ENABLE_XLIO=ON\n");
  return 2;
#endif
}
