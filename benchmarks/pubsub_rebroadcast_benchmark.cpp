// End-to-end latency through two Goblin Core servers joined by an SBE ring.
// Every measured connection uses SBE over a shared-memory ring.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using goblin::core::PubSubKind;
using goblin::core::SbeRingClient;

namespace {

struct Config {
  fs::path server;
  std::size_t samples{1'000};
  std::size_t warmup{100};
  std::string ring_bytes{"2mb"};
  int controller_cpu{2};
  int relay_cpu{3};
  int benchmark_cpu{4};
};

[[nodiscard]] std::size_t parse_size(std::string_view text,
                                     std::string_view option) {
  std::size_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
    throw std::runtime_error("invalid positive value for " + std::string(option));
  }
  return value;
}

[[nodiscard]] int parse_cpu(std::string_view text, std::string_view option) {
  int value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
    throw std::runtime_error("invalid CPU for " + std::string(option));
  }
  return value;
}

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --server PATH [options]\n"
      << "  --samples N          measured messages (default: 1000)\n"
      << "  --warmup N           unmeasured messages (default: 100)\n"
      << "  --ring-bytes SIZE    each ring, per direction (default: 2mb)\n"
      << "  --controller-cpu N   controller server CPU (default: 2)\n"
      << "  --relay-cpu N        rebroadcasting server CPU (default: 3)\n"
      << "  --benchmark-cpu N    publisher/subscriber CPU (default: 4)\n";
}

[[nodiscard]] Config parse_arguments(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    const auto value = [&](std::string_view name) -> std::string_view {
      if (++i >= argc) {
        throw std::runtime_error(std::string(name) + " requires a value");
      }
      return argv[i];
    };
    if (option == "--server") {
      config.server = value(option);
    } else if (option == "--samples") {
      config.samples = parse_size(value(option), option);
    } else if (option == "--warmup") {
      config.warmup = parse_size(value(option), option);
    } else if (option == "--ring-bytes") {
      config.ring_bytes = value(option);
    } else if (option == "--controller-cpu") {
      config.controller_cpu = parse_cpu(value(option), option);
    } else if (option == "--relay-cpu") {
      config.relay_cpu = parse_cpu(value(option), option);
    } else if (option == "--benchmark-cpu") {
      config.benchmark_cpu = parse_cpu(value(option), option);
    } else if (option == "--help" || option == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + std::string(option));
    }
  }
  if (config.server.empty()) {
    throw std::runtime_error("--server is required");
  }
  if (!fs::exists(config.server)) {
    throw std::runtime_error("server binary does not exist: " +
                             config.server.string());
  }
  return config;
}

void pin_to_cpu(int cpu) noexcept {
#ifdef __linux__
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)::sched_setaffinity(0, sizeof(set), &set);
#else
  (void)cpu;
#endif
}

[[nodiscard]] std::string read_file(const fs::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

class ServerProcess {
 public:
  ServerProcess(const Config& config, std::string role,
                std::vector<std::string> arguments, int cpu)
      : role_(std::move(role)) {
    const auto tag = std::to_string(::getpid()) + '-' + role_;
    log_path_ = "/tmp/goblin-pubsub-relay-bench-" + tag + ".log";
    socket_path_ = "/tmp/goblin-pubsub-relay-bench-" + tag + ".sock";
    (void)::unlink(log_path_.c_str());
    (void)::unlink(socket_path_.c_str());

    std::vector<std::string> command;
#ifdef __linux__
    command = {"/usr/bin/taskset", "-c", std::to_string(cpu),
               config.server.string(), "--unixsocket", socket_path_};
#else
    (void)cpu;
    command = {config.server.string(), "--unixsocket", socket_path_};
#endif
    command.insert(command.end(), std::make_move_iterator(arguments.begin()),
                   std::make_move_iterator(arguments.end()));

    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid_ == 0) {
      const int log =
          ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (log >= 0) {
        (void)::dup2(log, STDOUT_FILENO);
        (void)::dup2(log, STDERR_FILENO);
        if (log > STDERR_FILENO) {
          (void)::close(log);
        }
      }
      std::vector<char*> argv;
      argv.reserve(command.size() + 1);
      for (auto& argument : command) {
        argv.push_back(argument.data());
      }
      argv.push_back(nullptr);
      ::execv(argv.front(), argv.data());
      _exit(127);
    }
  }

  ~ServerProcess() { stop(); }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  void assert_running() const {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == pid_) {
      throw std::runtime_error(role_ + " server exited during startup\n" +
                               read_file(log_path_));
    }
  }

  [[nodiscard]] std::string log() const { return read_file(log_path_); }

 private:
  void stop() noexcept {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGTERM);
      for (int i = 0; i < 200; ++i) {
        int status = 0;
        if (::waitpid(pid_, &status, WNOHANG) == pid_) {
          pid_ = -1;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (pid_ > 0) {
        (void)::kill(pid_, SIGKILL);
        (void)::waitpid(pid_, nullptr, 0);
        pid_ = -1;
      }
    }
    (void)::unlink(socket_path_.c_str());
    (void)::unlink(log_path_.c_str());
  }

  std::string role_;
  pid_t pid_{-1};
  std::string log_path_;
  std::string socket_path_;
};

[[nodiscard]] SbeRingClient open_ring(std::string_view path,
                                      const ServerProcess& server) {
  auto client =
      SbeRingClient::open(std::string(path).c_str(), std::chrono::seconds(20));
  if (!client) {
    server.assert_running();
    throw std::runtime_error("could not open ring " + std::string(path) +
                             "\nserver log:\n" + server.log());
  }
  if (!client->ping(std::chrono::seconds(5))) {
    throw std::runtime_error("server did not answer SBE PING on " +
                             std::string(path));
  }
  return std::move(*client);
}

[[nodiscard]] std::string payload_for(std::size_t iteration) {
  return "m:" + std::to_string(iteration);
}

void publish_and_verify(SbeRingClient& publisher, SbeRingClient& subscriber,
                        std::string_view channel, std::string_view payload) {
  const auto delivered =
      publisher.publish(channel, payload, std::chrono::seconds(5));
  if (delivered != 1) {
    throw std::runtime_error("controller PUBLISH returned " +
                             std::to_string(delivered) + ", expected 1");
  }
  auto message = subscriber.read_pubsub(std::chrono::seconds(5));
  if (message.kind != PubSubKind::message || message.channel != channel ||
      message.payload != payload) {
    throw std::runtime_error("subscriber received the wrong relayed message");
  }
}

struct Distribution {
  double minimum_us{0};
  double p50_us{0};
  double p90_us{0};
  double p99_us{0};
  double p999_us{0};
  double maximum_us{0};
  double mean_us{0};
};

[[nodiscard]] Distribution summarize(std::vector<std::uint64_t> ns) {
  std::sort(ns.begin(), ns.end());
  const auto percentile = [&](double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(ns.size())));
    return static_cast<double>(ns[std::max<std::size_t>(1, rank) - 1]) / 1000.0;
  };
  const long double total =
      std::accumulate(ns.begin(), ns.end(), static_cast<long double>(0));
  return Distribution{
      static_cast<double>(ns.front()) / 1000.0,
      percentile(0.50),
      percentile(0.90),
      percentile(0.99),
      percentile(0.999),
      static_cast<double>(ns.back()) / 1000.0,
      static_cast<double>(total / static_cast<long double>(ns.size())) / 1000.0};
}

void unlink_path(const std::string& path) noexcept {
  (void)::unlink(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::signal(SIGPIPE, SIG_IGN);
    const auto config = parse_arguments(argc, argv);
    const auto tag = std::to_string(::getpid());
    const std::string listener_ring =
        "/tmp/goblin-pubsub-relay-bench-" + tag + "-listener.ring";
    const std::string publisher_ring =
        "/tmp/goblin-pubsub-relay-bench-" + tag + "-publisher.ring";
    const std::string subscriber_ring =
        "/tmp/goblin-pubsub-relay-bench-" + tag + "-subscriber.ring";
    for (const auto* path : {&listener_ring, &publisher_ring, &subscriber_ring}) {
      unlink_path(*path);
    }

    ServerProcess controller(
        config, "controller",
        {"--ring", listener_ring, config.ring_bytes, "--ring", publisher_ring,
         config.ring_bytes},
        config.controller_cpu);
    auto publisher = open_ring(publisher_ring, controller);

    ServerProcess relay(
        config, "relay",
        {"--ring", subscriber_ring, config.ring_bytes,
         "--pubsub-listener-ring", listener_ring},
        config.relay_cpu);
    auto subscriber = open_ring(subscriber_ring, relay);
    constexpr std::string_view channel = "bench:relay";
    const std::string_view channels[]{channel};
    const auto acknowledgements =
        subscriber.subscribe(channels, std::chrono::seconds(5));
    if (acknowledgements.size() != 1) {
      throw std::runtime_error("local SBE SUBSCRIBE did not return one acknowledgement");
    }

    pin_to_cpu(config.benchmark_cpu);
    for (std::size_t i = 0; i < config.warmup; ++i) {
      const auto payload = payload_for(i);
      publish_and_verify(publisher, subscriber, channel, payload);
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(config.samples);
    const auto run_start = Clock::now();
    for (std::size_t i = 0; i < config.samples; ++i) {
      const auto payload = payload_for(config.warmup + i);
      const auto start = Clock::now();
      publish_and_verify(publisher, subscriber, channel, payload);
      const auto end = Clock::now();
      latencies.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
              .count()));
    }
    const auto run_end = Clock::now();
    const auto distribution = summarize(std::move(latencies));
    const double seconds = std::chrono::duration<double>(run_end - run_start).count();

    std::cout << "Goblin Core two-server Pub/Sub rebroadcast latency\n"
              << "path: SBE publisher ring -> controller -> SBE listener ring -> "
                 "relay -> SBE subscriber ring\n"
              << "messages: " << config.samples << " measured, " << config.warmup
              << " warmup; payloads: " << payload_for(0).size() << "-"
              << payload_for(config.warmup + config.samples - 1).size()
              << " bytes; channel: " << channel << '\n'
              << "ring buffers: 3 x " << config.ring_bytes
              << " requested bytes per direction\n"
              << "CPUs: controller=" << config.controller_cpu
              << ", relay=" << config.relay_cpu
              << ", benchmark=" << config.benchmark_cpu << '\n'
              << std::fixed << std::setprecision(3)
              << "latency_us: min=" << distribution.minimum_us
              << " mean=" << distribution.mean_us << " p50=" << distribution.p50_us
              << " p90=" << distribution.p90_us << " p99=" << distribution.p99_us
              << " p99.9=" << distribution.p999_us
              << " max=" << distribution.maximum_us << '\n'
              << std::setprecision(0) << "sequential_messages_per_second: "
              << static_cast<double>(config.samples) / seconds << '\n';

    for (const auto* path : {&subscriber_ring, &publisher_ring, &listener_ring}) {
      unlink_path(*path);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pubsub_rebroadcast_benchmark: " << error.what() << '\n';
    print_usage(argv[0]);
    return 2;
  }
}
