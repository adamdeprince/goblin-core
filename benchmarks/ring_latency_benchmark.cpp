// Native depth-one Goblin *local shared-memory ring* latency probe.
//
// Same ops as benchmarks/rdma_latency_benchmark.cpp (so RDMA vs local ring is
// an apples-to-apples comparison):
//   SBE PING, RESP PING, SET, GET, HSET, HGET
//
// Run against a goblin-core started with --ring PATH SIZE. Client runs on the
// same host (see benchmarks/ring_local_latency.sh). Pin server/client to
// adjacent cores for best numbers.

#include "goblin/core/ring_client.hpp"
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
      std::cerr << "ring_latency_benchmark: " << label
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
      std::cerr << "ring_latency_benchmark: " << label
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
[[nodiscard]] bool is_value_reply(std::string_view reply) {
  return !reply.empty() && (reply[0] == '$' || reply[0] == ':');
}
[[nodiscard]] bool is_hset_reply(std::string_view reply) {
  return !reply.empty() && reply[0] == ':';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "usage: " << argv[0]
              << " RING-PATH [SAMPLES] [WARMUP]\n"
              << "  Depth-one local shared-memory ring latency:\n"
              << "  SBE PING, RESP PING, SET, GET, HSET, HGET\n"
              << "  (same op set as goblin_core_rdma_latency_benchmark)\n";
    return 2;
  }

  std::size_t samples = 200'000;
  std::size_t warmup = 20'000;
  if ((argc >= 3 && !parse_integer<std::size_t>(argv[2], samples)) ||
      (argc == 4 && !parse_integer<std::size_t>(argv[3], warmup)) ||
      samples == 0) {
    std::cerr << "ring_latency_benchmark: invalid argument\n";
    return 2;
  }

  const char* path = argv[1];
  calibrate_ticks();

  std::cout << "Depth-one local ring round trips path=" << path << ", "
            << samples << " measured after " << warmup
            << " warmup operations\n"
            << "(one client at a time: shared-memory rings are exclusive)\n";

  // SBE client first (exclusive ring owner).
  {
    auto sbe = goblin::core::SbeRingClient::open(
        path, std::chrono::milliseconds(5000));
    if (!sbe) {
      std::cerr << "ring_latency_benchmark: SbeRingClient open failed for "
                << path << '\n';
      return 1;
    }
    if (!measure("SBE/ring PING", warmup, samples,
                 [&] { return sbe->ping(); })) {
      return 1;
    }
  }  // drop SBE client before RESP claims the ring

  // RESP client second.
  {
    auto resp = goblin::core::ring::RingClient::open(
        path, std::chrono::milliseconds(5000));
    if (!resp) {
      std::cerr << "ring_latency_benchmark: RESP RingClient open failed for "
                << path << '\n';
      return 1;
    }

    {
      const auto set_reply = resp->command({"SET", "ring:lat:str", "v1"});
      if (!set_reply || !is_ok_simple(*set_reply)) {
        std::cerr << "ring_latency_benchmark: seed SET failed\n";
        return 1;
      }
      const auto hset_reply =
          resp->command({"HSET", "ring:lat:hash", "f", "1"});
      if (!hset_reply || !is_hset_reply(*hset_reply)) {
        std::cerr << "ring_latency_benchmark: seed HSET failed\n";
        return 1;
      }
    }

    if (!measure("RESP/ring PING", warmup, samples, [&] {
          const auto reply = resp->command({"PING"});
          return reply && is_pong(*reply);
        })) {
      return 1;
    }
    if (!measure("RESP/ring SET", warmup, samples, [&] {
          const auto reply = resp->command({"SET", "ring:lat:str", "v1"});
          return reply && is_ok_simple(*reply);
        })) {
      return 1;
    }
    if (!measure("RESP/ring GET", warmup, samples, [&] {
          const auto reply = resp->command({"GET", "ring:lat:str"});
          return reply && is_value_reply(*reply);
        })) {
      return 1;
    }
    if (!measure("RESP/ring HSET", warmup, samples, [&] {
          const auto reply =
              resp->command({"HSET", "ring:lat:hash", "f", "1"});
          return reply && is_hset_reply(*reply);
        })) {
      return 1;
    }
    if (!measure("RESP/ring HGET", warmup, samples, [&] {
          const auto reply = resp->command({"HGET", "ring:lat:hash", "f"});
          return reply && is_value_reply(*reply);
        })) {
      return 1;
    }
  }
  return 0;
}
