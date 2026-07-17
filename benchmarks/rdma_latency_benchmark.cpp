// Native depth-one Goblin RDMA latency probe.
//
// Measures unpipelined round trips over the polled RDMA ring for basic ops:
//   SBE PING, RESP PING, SET, GET, HSET, HGET
//
// Run the client on a host local to its HCA; pin both server and client to
// NUMA-local cores (see benchmarks/rdma_thunder_butterfly.sh).

#include "goblin/core/rdma_client.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
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

[[nodiscard]] double percentile(const std::vector<std::uint64_t>& values,
                                double fraction) {
  const auto index = static_cast<std::size_t>(
      fraction * static_cast<double>(values.size() - 1));
  return static_cast<double>(values[index]) * nanoseconds_per_tick / 1000.0;
}

template <class Operation>
[[nodiscard]] bool measure(std::string_view label, std::size_t warmup,
                           std::size_t samples, Operation&& operation) {
  for (std::size_t i = 0; i < warmup; ++i) {
    if (!operation()) {
      std::cerr << "rdma_latency_benchmark: " << label
                << " failed during warmup\n";
      return false;
    }
  }

  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const auto begin = hardware_ticks();
    const bool ok = operation();
    const auto end = hardware_ticks();
    if (!ok) {
      std::cerr << "rdma_latency_benchmark: " << label
                << " failed at sample " << i << '\n';
      return false;
    }
    elapsed.push_back(end - begin);
  }
  std::ranges::sort(elapsed);
  const double total_ticks = std::accumulate(
      elapsed.begin(), elapsed.end(), 0.0,
      [](double total, std::uint64_t value) { return total + value; });
  const double mean_us =
      total_ticks * nanoseconds_per_tick /
      static_cast<double>(elapsed.size()) / 1000.0;
  const double operations_per_second = 1.0e6 / mean_us;

  std::cout << std::left << std::setw(22) << label << std::right << std::fixed
            << std::setprecision(3) << " min " << percentile(elapsed, 0.0)
            << " us  p50 " << percentile(elapsed, 0.50) << " us  p90 "
            << percentile(elapsed, 0.90) << " us  p99 "
            << percentile(elapsed, 0.99) << " us  p99.9 "
            << percentile(elapsed, 0.999) << " us  mean " << mean_us
            << " us  " << std::setprecision(0) << operations_per_second
            << " ops/s\n";
  return true;
}

[[nodiscard]] bool is_ok_simple(std::string_view reply) {
  return reply == "+OK\r\n";
}

[[nodiscard]] bool is_pong(std::string_view reply) {
  return reply == "+PONG\r\n";
}

// RESP bulk string or integer reply starts with $ or : — enough for GET/HGET.
[[nodiscard]] bool is_value_reply(std::string_view reply) {
  return !reply.empty() && (reply[0] == '$' || reply[0] == ':');
}

[[nodiscard]] bool is_hset_reply(std::string_view reply) {
  // HSET returns integer (fields added): :0\r\n or :1\r\n after first write.
  return !reply.empty() && reply[0] == ':';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 6) {
    std::cerr << "usage: " << argv[0]
              << " ADDRESS PORT RING-SIZE [SAMPLES] [WARMUP]\n"
              << "  Depth-one RDMA latency: PING, SET/GET, HSET/HGET\n";
    return 2;
  }

  unsigned port_value = 0;
  std::size_t samples = 200'000;
  std::size_t warmup = 20'000;
  const auto ring_bytes = goblin::core::ring::parse_size(argv[3]);
  if (!parse_integer<unsigned>(argv[2], port_value) || port_value == 0 ||
      port_value > 65535 || !ring_bytes || *ring_bytes == 0 ||
      (argc >= 5 && !parse_integer<std::size_t>(argv[4], samples)) ||
      (argc == 6 && !parse_integer<std::size_t>(argv[5], warmup)) ||
      samples == 0) {
    std::cerr << "rdma_latency_benchmark: invalid argument\n";
    return 2;
  }

  calibrate_ticks();
  const auto port = static_cast<std::uint16_t>(port_value);
  std::string error;
  auto resp = goblin::core::rdma::RdmaClient::open(
      argv[1], port, *ring_bytes, std::chrono::seconds(5), &error);
  if (!resp) {
    std::cerr << "rdma_latency_benchmark: RESP connect: " << error << '\n';
    return 1;
  }
  auto sbe = goblin::core::SbeRdmaClient::open(
      std::string_view(argv[1]), port, *ring_bytes, std::chrono::seconds(5),
      16 * 1024, &error);
  if (!sbe) {
    std::cerr << "rdma_latency_benchmark: SBE connect: " << error << '\n';
    return 1;
  }

  // Seed keys once so GET/HGET hit existing values.
  {
    const auto set_reply = resp->command({"SET", "rdma:lat:str", "v1"});
    if (!set_reply || !is_ok_simple(*set_reply)) {
      std::cerr << "rdma_latency_benchmark: seed SET failed\n";
      return 1;
    }
    const auto hset_reply =
        resp->command({"HSET", "rdma:lat:hash", "f", "1"});
    if (!hset_reply || !is_hset_reply(*hset_reply)) {
      std::cerr << "rdma_latency_benchmark: seed HSET failed\n";
      return 1;
    }
  }

  std::cout << "Depth-one RDMA round trips to " << argv[1] << ":" << port
            << " ring=" << *ring_bytes << " B, " << samples
            << " measured after " << warmup << " warmup operations\n";

  if (!measure("SBE/RDMA PING", warmup, samples,
               [&] { return sbe->ping(); })) {
    return 1;
  }
  if (!measure("RESP/RDMA PING", warmup, samples, [&] {
        const auto reply = resp->command({"PING"});
        return reply && is_pong(*reply);
      })) {
    return 1;
  }
  if (!measure("RESP/RDMA SET", warmup, samples, [&] {
        const auto reply = resp->command({"SET", "rdma:lat:str", "v1"});
        return reply && is_ok_simple(*reply);
      })) {
    return 1;
  }
  if (!measure("RESP/RDMA GET", warmup, samples, [&] {
        const auto reply = resp->command({"GET", "rdma:lat:str"});
        return reply && is_value_reply(*reply);
      })) {
    return 1;
  }
  if (!measure("RESP/RDMA HSET", warmup, samples, [&] {
        const auto reply =
            resp->command({"HSET", "rdma:lat:hash", "f", "1"});
        return reply && is_hset_reply(*reply);
      })) {
    return 1;
  }
  if (!measure("RESP/RDMA HGET", warmup, samples, [&] {
        const auto reply =
            resp->command({"HGET", "rdma:lat:hash", "f"});
        return reply && is_value_reply(*reply);
      })) {
    return 1;
  }
  return 0;
}
