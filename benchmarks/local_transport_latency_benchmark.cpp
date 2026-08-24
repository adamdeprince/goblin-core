// Depth-one local transport latency for Goblin's RESP and SBE protocols.
//
// The companion local_transport_latency.sh script starts one isolated server
// for each case. This probe then drives identical PING, SET, and GET round
// trips over a shared-memory ring, Aeron IPC, Aeron loopback UDP, or a Unix
// domain socket. Every mode uses the same hardware-tick timer and percentile
// calculation so protocol and transport are the only intended variables.

#include "goblin/core/aeron_client.hpp"
#include "goblin/core/aeron_transport.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_ring_client.hpp"
#include "goblin/core/sbe_socket_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#endif

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view kKey{"goblin:local-transport-latency"};
constexpr std::string_view kValue{"0123456789abcdef"};

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
  return static_cast<std::uint64_t>(
      Clock::now().time_since_epoch().count());
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
      std::chrono::duration<double, std::nano>(finish_time - start_time)
          .count() /
      static_cast<double>(finish_ticks - start_ticks);
#endif
}

template <class Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

template <class Integer>
[[nodiscard]] Integer environment_integer(const char* name,
                                          Integer fallback) {
  const char* text = std::getenv(name);
  if (text == nullptr || *text == '\0') return fallback;
  Integer value{};
  if (!parse_integer<Integer>(text, value)) {
    throw std::runtime_error(std::string("invalid ") + name + " value: " +
                             text);
  }
  return value;
}

#ifdef __linux__
[[nodiscard]] bool pin_thread(pid_t tid, int cpu) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::sched_setaffinity(tid, sizeof(set), &set) == 0;
}

void pin_client_threads() {
  const int client_cpu = environment_integer<int>("CLIENT_CPU", -1);
  const int aeron_cpu = environment_integer<int>("AERON_CLIENT_CPU", -1);
  const auto current = static_cast<pid_t>(::syscall(SYS_gettid));

  // Aeron's client conductor exists by the time this function is called. Keep
  // it off the measuring core when the launcher supplies AERON_CLIENT_CPU.
  if (aeron_cpu >= 0) {
    if (DIR* tasks = ::opendir("/proc/self/task"); tasks != nullptr) {
      while (const dirent* entry = ::readdir(tasks)) {
        pid_t tid = 0;
        if (!parse_integer<pid_t>(entry->d_name, tid) || tid == current) {
          continue;
        }
        if (!pin_thread(tid, aeron_cpu)) {
          std::cerr << "local_transport_latency: warning: cannot pin helper "
                    << tid << " to CPU " << aeron_cpu << '\n';
        }
      }
      ::closedir(tasks);
    }
  }
  if (client_cpu >= 0 && !pin_thread(0, client_cpu)) {
    throw std::runtime_error("cannot pin client thread to CPU " +
                             std::to_string(client_cpu));
  }
}
#else
void pin_client_threads() {}
#endif

[[nodiscard]] double microseconds(const std::vector<std::uint64_t>& values,
                                  double fraction) {
  const auto index = static_cast<std::size_t>(
      fraction * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]) * nanoseconds_per_tick / 1000.0;
}

template <class Operation>
void measure(std::string_view transport, std::string_view protocol,
             std::string_view operation_name, std::size_t warmup,
             std::size_t samples, Operation&& operation) {
  for (std::size_t i = 0; i < warmup; ++i) {
    if (!operation()) {
      throw std::runtime_error(std::string(operation_name) +
                               " failed during warmup");
    }
  }

  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto begin = hardware_ticks();
    const bool ok = operation();
    const auto end = hardware_ticks();
    if (!ok) {
      throw std::runtime_error(std::string(operation_name) +
                               " failed at sample " + std::to_string(i));
    }
    elapsed.push_back(end - begin);
  }

  std::ranges::sort(elapsed);
  const double total_ticks = std::accumulate(
      elapsed.begin(), elapsed.end(), 0.0,
      [](double total, std::uint64_t value) { return total + value; });
  const double mean = total_ticks * nanoseconds_per_tick /
                      static_cast<double>(samples) / 1000.0;
  const double minimum = microseconds(elapsed, 0.0);
  const double p50 = microseconds(elapsed, 0.50);
  const double p90 = microseconds(elapsed, 0.90);
  const double p99 = microseconds(elapsed, 0.99);
  const double p999 = microseconds(elapsed, 0.999);

  std::cout << "LAT," << transport << ',' << protocol << ',' << operation_name
            << ',' << std::fixed << std::setprecision(4) << minimum << ','
            << p50 << ',' << p90 << ',' << p99 << ',' << p999 << ',' << mean
            << ',' << samples << '\n';
  std::cerr << std::left << std::setw(17)
            << (std::string(protocol) + '/' + std::string(transport))
            << std::setw(6) << operation_name << std::right << std::fixed
            << std::setprecision(3) << " min " << minimum << " us  p50 "
            << p50 << " us  p90 " << p90 << " us  p99 " << p99
            << " us  p99.9 " << p999 << " us  mean " << mean << " us\n";
}

class UdsRespClient {
 public:
  UdsRespClient(const UdsRespClient&) = delete;
  UdsRespClient& operator=(const UdsRespClient&) = delete;
  UdsRespClient(UdsRespClient&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)), pending_(std::move(other.pending_)) {}
  UdsRespClient& operator=(UdsRespClient&&) = delete;
  ~UdsRespClient() {
    if (fd_ >= 0) (void)::close(fd_);
  }

  [[nodiscard]] static std::optional<UdsRespClient> open(
      std::string_view path, std::string& error) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
      error = "Unix-domain socket path is too long";
      return std::nullopt;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      error = std::strerror(errno);
      return std::nullopt;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) != 0) {
      error = std::strerror(errno);
      (void)::close(fd);
      return std::nullopt;
    }
    return UdsRespClient(fd);
  }

  void send_raw(std::string_view bytes) {
    while (!bytes.empty()) {
      const ssize_t sent =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
        continue;
      }
      if (sent < 0 && errno == EINTR) continue;
      throw std::runtime_error(std::string("UDS send: ") +
                               std::strerror(errno));
    }
  }

  [[nodiscard]] std::optional<std::string> read_reply() {
    for (;;) {
      if (const auto end = goblin::core::ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      char bytes[16U * 1024U];
      const ssize_t received = ::recv(fd_, bytes, sizeof(bytes), 0);
      if (received > 0) {
        pending_.append(bytes, static_cast<std::size_t>(received));
        continue;
      }
      if (received < 0 && errno == EINTR) continue;
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> arguments) {
    send_raw(goblin::core::ring::encode_command(arguments));
    return read_reply();
  }

 private:
  explicit UdsRespClient(int fd) noexcept : fd_(fd) { pending_.reserve(8192); }

  int fd_{-1};
  std::string pending_;
};

template <class Client>
void run_resp(Client& client, std::string_view transport, std::size_t warmup,
              std::size_t samples) {
  const std::array<std::string_view, 1> ping_args{"PING"};
  const std::array<std::string_view, 3> set_args{"SET", kKey, kValue};
  const std::array<std::string_view, 2> get_args{"GET", kKey};
  const std::string value_reply = "$" + std::to_string(kValue.size()) +
                                  "\r\n" + std::string(kValue) + "\r\n";

  const auto round_trip = [&](std::span<const std::string_view> arguments,
                              std::string_view expected) {
    const auto reply = client.command(arguments);
    return reply && *reply == expected;
  };
  if (!round_trip(set_args, "+OK\r\n")) {
    throw std::runtime_error("RESP seed SET failed");
  }

  measure(transport, "RESP", "PING", warmup, samples,
          [&] { return round_trip(ping_args, "+PONG\r\n"); });
  measure(transport, "RESP", "SET", warmup, samples,
          [&] { return round_trip(set_args, "+OK\r\n"); });
  measure(transport, "RESP", "GET", warmup, samples,
          [&] { return round_trip(get_args, value_reply); });
}

template <class Client>
void run_sbe(Client& client, std::string_view transport, std::size_t warmup,
             std::size_t samples) {
  const auto seeded = client.set(kKey, kValue);
  if (!seeded.ok) throw std::runtime_error("SBE seed SET failed");

  measure(transport, "SBE", "PING", warmup, samples,
          [&] { return client.ping(); });
  measure(transport, "SBE", "SET", warmup, samples, [&] {
    return client.set(kKey, kValue).ok;
  });
  measure(transport, "SBE", "GET", warmup, samples, [&] {
    const auto value = client.get(kKey);
    return value && *value == kValue;
  });
}

[[noreturn]] void usage(const char* program) {
  std::cerr
      << "usage:\n"
      << "  " << program << " ring-resp PATH\n"
      << "  " << program << " ring-sbe PATH\n"
      << "  " << program << " uds-resp PATH\n"
      << "  " << program << " uds-sbe PATH\n"
      << "  " << program
      << " aeron-ipc-resp AERON-DIR REQUEST-STREAM RESPONSE-STREAM\n"
      << "  " << program
      << " aeron-ipc-sbe AERON-DIR REQUEST-STREAM RESPONSE-STREAM\n"
      << "  " << program
      << " aeron-udp-resp AERON-DIR REQUEST-ENDPOINT REQUEST-STREAM "
         "RESPONSE-ENDPOINT RESPONSE-STREAM\n"
      << "  " << program
      << " aeron-udp-sbe AERON-DIR REQUEST-ENDPOINT REQUEST-STREAM "
         "RESPONSE-ENDPOINT RESPONSE-STREAM\n\n"
      << "Environment: SAMPLES=100000 WARMUP=10000 CLIENT_CPU=N "
         "AERON_CLIENT_CPU=N\n";
  std::exit(2);
}

[[nodiscard]] std::int32_t stream_id(std::string_view text) {
  std::int32_t value = 0;
  if (!parse_integer(text, value)) {
    throw std::runtime_error("invalid Aeron stream id: " + std::string(text));
  }
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3) usage(argv[0]);
    const std::string_view mode = argv[1];
    const std::size_t samples =
        environment_integer<std::size_t>("SAMPLES", 100'000);
    const std::size_t warmup =
        environment_integer<std::size_t>("WARMUP", 10'000);
    if (samples == 0) throw std::runtime_error("SAMPLES must be positive");

    if (mode == "ring-resp" && argc == 3) {
      auto client = goblin::core::ring::RingClient::open(argv[2], 5s);
      if (!client) throw std::runtime_error("cannot open RESP ring");
      pin_client_threads();
      calibrate_ticks();
      run_resp(*client, "ring", warmup, samples);
      return 0;
    }
    if (mode == "ring-sbe" && argc == 3) {
      auto client = goblin::core::SbeRingClient::open(argv[2], 5s);
      if (!client) throw std::runtime_error("cannot open SBE ring");
      pin_client_threads();
      calibrate_ticks();
      run_sbe(*client, "ring", warmup, samples);
      return 0;
    }
    if (mode == "uds-resp" && argc == 3) {
      std::string error;
      auto client = UdsRespClient::open(argv[2], error);
      if (!client) throw std::runtime_error("cannot open RESP UDS: " + error);
      pin_client_threads();
      calibrate_ticks();
      run_resp(*client, "uds", warmup, samples);
      return 0;
    }
    if (mode == "uds-sbe" && argc == 3) {
      std::string error;
      auto client = goblin::core::SbeSocketClient::open(
          goblin::core::SbeSocketEndpoint::unix_domain(argv[2]), 5s,
          64U * 1024U, &error);
      if (!client) throw std::runtime_error("cannot open SBE UDS: " + error);
      pin_client_threads();
      calibrate_ticks();
      run_sbe(*client, "uds", warmup, samples);
      return 0;
    }

    const bool ipc_resp = mode == "aeron-ipc-resp";
    const bool ipc_sbe = mode == "aeron-ipc-sbe";
    if ((ipc_resp || ipc_sbe) && argc == 5) {
      const auto channels = goblin::core::aeron::ChannelConfig::ipc(
          stream_id(argv[3]), stream_id(argv[4]));
      std::string error;
      if (ipc_resp) {
        auto client = goblin::core::aeron::AeronClient::open(
            channels, 5s, argv[2], &error);
        if (!client) {
          throw std::runtime_error("cannot open RESP Aeron IPC: " + error);
        }
        pin_client_threads();
        calibrate_ticks();
        run_resp(*client, "aeron-ipc", warmup, samples);
      } else {
        auto client = goblin::core::SbeAeronClient::open(
            channels, 5s, 64U * 1024U, argv[2], &error);
        if (!client) {
          throw std::runtime_error("cannot open SBE Aeron IPC: " + error);
        }
        pin_client_threads();
        calibrate_ticks();
        run_sbe(*client, "aeron-ipc", warmup, samples);
      }
      return 0;
    }

    const bool udp_resp = mode == "aeron-udp-resp";
    const bool udp_sbe = mode == "aeron-udp-sbe";
    if ((udp_resp || udp_sbe) && argc == 7) {
      const auto channels = goblin::core::aeron::ChannelConfig::udp(
          argv[3], stream_id(argv[4]), argv[5], stream_id(argv[6]));
      std::string error;
      if (udp_resp) {
        auto client = goblin::core::aeron::AeronClient::open(
            channels, 5s, argv[2], &error);
        if (!client) {
          throw std::runtime_error("cannot open RESP Aeron UDP: " + error);
        }
        pin_client_threads();
        calibrate_ticks();
        run_resp(*client, "aeron-udp", warmup, samples);
      } else {
        auto client = goblin::core::SbeAeronClient::open(
            channels, 5s, 64U * 1024U, argv[2], &error);
        if (!client) {
          throw std::runtime_error("cannot open SBE Aeron UDP: " + error);
        }
        pin_client_threads();
        calibrate_ticks();
        run_sbe(*client, "aeron-udp", warmup, samples);
      }
      return 0;
    }
    usage(argv[0]);
  } catch (const std::exception& error) {
    std::cerr << "local_transport_latency: " << error.what() << '\n';
    return 1;
  }
}
