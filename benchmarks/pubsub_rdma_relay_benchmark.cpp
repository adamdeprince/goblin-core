// End-to-end Pub/Sub relay latency across two Goblin Core servers.
//
// The benchmark process runs beside the upstream server. It publishes through
// a local SBE ring, then receives the downstream server's rebroadcast through
// an SBE RDMA connection. A single local steady clock therefore covers the
// complete local-server -> fabric -> remote-server -> fabric return path.

#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using goblin::core::PubSubKind;
using goblin::core::SbeRdmaClient;
using goblin::core::SbeRingClient;

struct Config {
  std::string ring_path;
  std::string rdma_address;
  std::uint16_t rdma_port{0};
  std::uint64_t rdma_bytes{0};
  std::size_t samples{1'000'000};
  std::size_t warmup{10'000};
  std::string channel{"FOO"};
  int cpu{-1};
};

template <class Integer>
[[nodiscard]] Integer parse_integer(std::string_view text,
                                    std::string_view option) {
  Integer value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("invalid value for " + std::string(option));
  }
  return value;
}

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --ring PATH --rdma-address ADDRESS\n"
      << "       --rdma-port PORT --rdma-bytes SIZE [options]\n"
      << "  --samples N       measured messages (default: 1000000)\n"
      << "  --warmup N        unmeasured messages (default: 10000)\n"
      << "  --channel NAME    literal channel (default: FOO)\n"
      << "  --cpu N           pin this process to CPU N\n";
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

    if (option == "--ring") {
      config.ring_path = value(option);
    } else if (option == "--rdma-address") {
      config.rdma_address = value(option);
    } else if (option == "--rdma-port") {
      const auto port = parse_integer<unsigned>(value(option), option);
      if (port == 0 || port > 65535) {
        throw std::runtime_error("--rdma-port must be in 1..65535");
      }
      config.rdma_port = static_cast<std::uint16_t>(port);
    } else if (option == "--rdma-bytes") {
      const auto bytes = goblin::core::ring::parse_size(value(option));
      if (!bytes || *bytes == 0) {
        throw std::runtime_error("invalid --rdma-bytes size");
      }
      config.rdma_bytes = *bytes;
    } else if (option == "--samples") {
      config.samples = parse_integer<std::size_t>(value(option), option);
      if (config.samples == 0) {
        throw std::runtime_error("--samples must be positive");
      }
    } else if (option == "--warmup") {
      config.warmup = parse_integer<std::size_t>(value(option), option);
    } else if (option == "--channel") {
      config.channel = value(option);
      if (config.channel.empty()) {
        throw std::runtime_error("--channel must not be empty");
      }
    } else if (option == "--cpu") {
      config.cpu = parse_integer<int>(value(option), option);
      if (config.cpu < 0) {
        throw std::runtime_error("--cpu must be non-negative");
      }
    } else if (option == "--help" || option == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + std::string(option));
    }
  }

  if (config.ring_path.empty() || config.rdma_address.empty() ||
      config.rdma_port == 0 || config.rdma_bytes == 0) {
    throw std::runtime_error(
        "--ring, --rdma-address, --rdma-port, and --rdma-bytes are required");
  }
  return config;
}

void pin_to_cpu(int cpu) {
#ifdef __linux__
  if (cpu < 0) {
    return;
  }
  if (cpu >= CPU_SETSIZE) {
    throw std::runtime_error("--cpu exceeds CPU_SETSIZE");
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error(std::string("sched_setaffinity: ") +
                             std::strerror(errno));
  }
#else
  if (cpu >= 0) {
    throw std::runtime_error("--cpu is supported only on Linux");
  }
#endif
}

[[nodiscard]] std::string payload_for(std::size_t sequence) {
  return "msg:" + std::to_string(sequence);
}

void publish_and_receive(SbeRingClient& publisher, SbeRdmaClient& subscriber,
                         std::string_view channel, std::string_view payload) {
  const auto delivered =
      publisher.publish(channel, payload, std::chrono::seconds(5));
  if (delivered != 1) {
    throw std::runtime_error("upstream PUBLISH returned " +
                             std::to_string(delivered) + ", expected 1");
  }

  auto message = subscriber.read_pubsub(std::chrono::seconds(5));
  if (message.kind != PubSubKind::message || message.channel != channel ||
      message.payload != payload) {
    throw std::runtime_error("received an incorrect relayed Pub/Sub message");
  }
}

struct Distribution {
  double minimum_us{0};
  double mean_us{0};
  double p50_us{0};
  double p75_us{0};
  double p90_us{0};
  double p95_us{0};
  double p99_us{0};
  double p999_us{0};
  double p9999_us{0};
  double maximum_us{0};
};

[[nodiscard]] Distribution summarize(std::vector<std::uint64_t> ns) {
  std::ranges::sort(ns);
  const auto percentile = [&](double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(ns.size())));
    return static_cast<double>(ns[std::max<std::size_t>(1, rank) - 1]) /
           1000.0;
  };
  const long double total =
      std::accumulate(ns.begin(), ns.end(), static_cast<long double>(0));
  return Distribution{
      .minimum_us = static_cast<double>(ns.front()) / 1000.0,
      .mean_us =
          static_cast<double>(total / static_cast<long double>(ns.size())) /
          1000.0,
      .p50_us = percentile(0.50),
      .p75_us = percentile(0.75),
      .p90_us = percentile(0.90),
      .p95_us = percentile(0.95),
      .p99_us = percentile(0.99),
      .p999_us = percentile(0.999),
      .p9999_us = percentile(0.9999),
      .maximum_us = static_cast<double>(ns.back()) / 1000.0,
  };
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = parse_arguments(argc, argv);
    pin_to_cpu(config.cpu);

    auto publisher =
        SbeRingClient::open(config.ring_path.c_str(), std::chrono::seconds(20));
    if (!publisher || !publisher->ping(std::chrono::seconds(5))) {
      throw std::runtime_error("could not connect to the local SBE ring");
    }

    std::string rdma_error;
    auto subscriber = SbeRdmaClient::open(
        config.rdma_address, config.rdma_port, config.rdma_bytes,
        std::chrono::seconds(20), SbeRdmaClient::kDefaultBufferBytes,
        &rdma_error);
    if (!subscriber) {
      throw std::runtime_error("could not connect to the downstream RDMA "
                               "listener: " +
                               rdma_error);
    }
    if (!subscriber->ping(std::chrono::seconds(5))) {
      throw std::runtime_error("downstream RDMA SBE PING failed");
    }

    const std::string_view channels[]{config.channel};
    const auto acknowledgements =
        subscriber->subscribe(channels, std::chrono::seconds(5));
    if (acknowledgements.size() != 1 ||
        acknowledgements.front().kind != PubSubKind::subscribe) {
      throw std::runtime_error("downstream SBE SUBSCRIBE was not acknowledged");
    }

    for (std::size_t i = 0; i < config.warmup; ++i) {
      const auto payload = payload_for(i);
      publish_and_receive(*publisher, *subscriber, config.channel, payload);
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(config.samples);
    const auto run_start = Clock::now();
    for (std::size_t i = 0; i < config.samples; ++i) {
      const auto payload = payload_for(config.warmup + i);
      const auto start = Clock::now();
      publish_and_receive(*publisher, *subscriber, config.channel, payload);
      const auto finish = Clock::now();
      latencies.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
              .count()));
    }
    const auto run_finish = Clock::now();
    const auto distribution = summarize(std::move(latencies));
    const double seconds =
        std::chrono::duration<double>(run_finish - run_start).count();

    std::cout
        << "Goblin Core ring -> RDMA Pub/Sub relay -> RDMA latency\n"
        << "path: SBE/ring publisher -> upstream server -> SBE/RDMA -> "
           "downstream relay -> SBE/RDMA subscriber\n"
        << "messages: " << config.samples << " measured, " << config.warmup
        << " warmup; channel: " << config.channel << "; payload: "
        << payload_for(0).size() << '-'
        << payload_for(config.samples + config.warmup - 1).size() << " bytes\n"
        << "RDMA endpoint: " << config.rdma_address << ':' << config.rdma_port
        << "; registered ring: " << config.rdma_bytes << " bytes/direction\n"
        << "benchmark CPU: " << config.cpu << '\n'
        << std::fixed << std::setprecision(3)
        << "latency_us: min=" << distribution.minimum_us
        << " mean=" << distribution.mean_us << " p50=" << distribution.p50_us
        << " p75=" << distribution.p75_us << " p90=" << distribution.p90_us
        << " p95=" << distribution.p95_us << " p99=" << distribution.p99_us
        << " p99.9=" << distribution.p999_us
        << " p99.99=" << distribution.p9999_us
        << " max=" << distribution.maximum_us << '\n'
        << std::setprecision(0) << "sequential_messages_per_second: "
        << static_cast<double>(config.samples) / seconds << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pubsub_rdma_relay_benchmark: " << error.what() << '\n';
    print_usage(argv[0]);
    return 2;
  }
}
