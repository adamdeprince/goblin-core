// RESP vs SBE latency over the shared-memory ring, same server. Two rings on one
// server: RESP (ring_client.hpp) and the SBE binary wire (sbe_ring_client.hpp,
// switched on by the GOBL magic). Measures the pure protocol delta -- fixed framing
// overhead (PING), native-vs-parsed scores (ZADD 1 and 10 members), and typed vs
// text field/value writes (HSET 1 and 10 fields).
//
//   sbe_vs_resp_ring <path-to-goblin-core>

#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif
#if defined(__x86_64__)
#include <x86intrin.h>  // __rdtscp
#endif

using clk = std::chrono::steady_clock;
using goblin::core::SbeRingClient;
using goblin::core::ring::RingClient;

namespace {
clk::time_point after(double s) {
  return clk::now() + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(s));
}

// Per-op timing reads the CPU cycle counter, not clock_gettime: rdtscp on x86 is ~7 ns and
// cycle-resolution (the invariant TSC counts at a constant rate regardless of the core's
// boost clock), versus ~25 ns of call overhead and coarser quantization from steady_clock.
// arm64 reads the generic timer (cntvct_el0 -- only 24 MHz on Apple Silicon, so no finer
// there). Calibrated once against steady_clock on x86.
double g_ns_per_tick = 1.0;
[[gnu::always_inline]] inline std::uint64_t hw_ticks() {
#if defined(__x86_64__)
  unsigned aux;
  return __rdtscp(&aux);
#elif defined(__aarch64__)
  std::uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  return static_cast<std::uint64_t>(clk::now().time_since_epoch().count());
#endif
}
void calibrate_hw_ticks() {
#if defined(__aarch64__)
  std::uint64_t f;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  g_ns_per_tick = 1e9 / static_cast<double>(f);
#elif defined(__x86_64__)
  const auto c0 = clk::now();
  const std::uint64_t t0 = hw_ticks();
  while (clk::now() - c0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t t1 = hw_ticks();
  const auto c1 = clk::now();
  g_ns_per_tick = std::chrono::duration<double, std::nano>(c1 - c0).count() /
                  static_cast<double>(t1 - t0);
#endif
}

template <class Op>
void measure(const char* label, Op&& op) {
  for (auto w = after(0.3); clk::now() < w;) op();      // warmup
  std::vector<long long> ns;
  ns.reserve(8'000'000);
  for (auto end = after(3.0); clk::now() < end;) {
    const std::uint64_t a = hw_ticks();
    op();
    const std::uint64_t b = hw_ticks();
    ns.push_back(static_cast<long long>(static_cast<double>(b - a) * g_ns_per_tick));
  }
  std::sort(ns.begin(), ns.end());
  const std::size_t n = ns.size();
  const double mean = std::accumulate(ns.begin(), ns.end(), 0.0) / n / 1000.0;
  std::printf("%-18s  mean=%6.3f  p50=%6.3f  p90=%6.3f  p99=%6.3f  min=%6.3f us   (n=%zu)\n",
              label, mean, ns[n / 2] / 1000.0, ns[std::size_t(n * 0.90)] / 1000.0,
              ns[std::size_t(n * 0.99)] / 1000.0, ns[0] / 1000.0, n);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: sbe_vs_resp_ring <goblin-core>\n"); return 2; }
  const char* server = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string resp_ring = "/tmp/gcbench-resp-" + tag + ".ring";
  const std::string sbe_ring = "/tmp/gcbench-sbe-" + tag + ".ring";
  const std::string sock = "/tmp/gcbench-" + tag + ".sock";
  ::unlink(resp_ring.c_str());
  ::unlink(sbe_ring.c_str());

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
#ifdef __linux__
    // Pin the busy-polling server to core 2 (falls through if taskset is absent).
    ::execl("/usr/bin/taskset", "taskset", "-c", "2", server, "--unixsocket", sock.c_str(),
            "--ring", resp_ring.c_str(), "1mb", "--ring", sbe_ring.c_str(), "1mb",
            static_cast<char*>(nullptr));
#endif
    ::execl(server, server, "--unixsocket", sock.c_str(),
            "--ring", resp_ring.c_str(), "1mb",
            "--ring", sbe_ring.c_str(), "1mb", static_cast<char*>(nullptr));
    _exit(127);
  }
#ifdef __linux__
  { cpu_set_t set; CPU_ZERO(&set); CPU_SET(3, &set); (void)::sched_setaffinity(0, sizeof(set), &set); }
#endif
  // macOS: no core pinning -- raise the client's busy-poll priority too.
  goblin::core::ring::set_busy_poll_thread_realtime();
  calibrate_hw_ticks();  // calibrate the cycle counter on the (now-pinned) measuring core

  auto resp = RingClient::open(resp_ring.c_str(), std::chrono::seconds(5));
  auto sbe = SbeRingClient::open(sbe_ring.c_str(), std::chrono::seconds(5));
  assert(resp && sbe);

  std::puts("PING -- nothing to parse; measures pure framing overhead:");
  const std::vector<std::string_view> ping_cmd = {"PING"};
  measure("  RESP", [&] { (void)resp->command(ping_cmd); });
  measure("  SBE", [&] { (void)sbe->ping(); });

  std::puts("\nZADD, 1 member -- one score parsed (RESP) vs native double (SBE):");
  const std::vector<std::string_view> zadd1 = {"ZADD", "k", "1.5", "m0"};
  const std::vector<SbeRingClient::Scored> sbe1 = {{1.5, "m0"}};
  measure("  RESP", [&] { (void)resp->command(zadd1); });
  measure("  SBE", [&] { (void)sbe->zadd("k", sbe1); });

  std::puts("\nZADD, 10 members -- 10 scores parsed (RESP) vs 10 native doubles (SBE):");
  std::vector<std::string_view> zadd10 = {"ZADD", "k"};
  static const char* scores[10] = {"1.1","2.2","3.3","4.4","5.5","6.6","7.7","8.8","9.9","10.1"};
  static const char* mems[10]   = {"m0","m1","m2","m3","m4","m5","m6","m7","m8","m9"};
  for (int i = 0; i < 10; ++i) { zadd10.push_back(scores[i]); zadd10.push_back(mems[i]); }
  std::vector<SbeRingClient::Scored> sbe10;
  for (int i = 0; i < 10; ++i) sbe10.push_back({1.1 * (i + 1), mems[i]});
  measure("  RESP", [&] { (void)resp->command(zadd10); });
  measure("  SBE", [&] { (void)sbe->zadd("k", sbe10); });

  std::puts("\nHSET, 1 field -- one field/value pair (RESP text vs SBE typed):");
  std::vector<std::string_view> hset1 = {"HSET", "h", "f0", "v0"};
  std::vector<std::pair<std::string_view, std::string_view>> sbe_h1 = {{"f0", "v0"}};
  measure("  RESP", [&] { (void)resp->command(hset1); });
  measure("  SBE", [&] { (void)sbe->hset("h", sbe_h1); });

  std::puts("\nHSET, 10 fields -- ten field/value pairs:");
  std::vector<std::string_view> hset10 = {"HSET", "h"};
  static const char* hf[10] = {"f0","f1","f2","f3","f4","f5","f6","f7","f8","f9"};
  static const char* hv[10] = {"v0","v1","v2","v3","v4","v5","v6","v7","v8","v9"};
  for (int i = 0; i < 10; ++i) { hset10.push_back(hf[i]); hset10.push_back(hv[i]); }
  std::vector<std::pair<std::string_view, std::string_view>> sbe_h10;
  for (int i = 0; i < 10; ++i) sbe_h10.push_back({hf[i], hv[i]});
  measure("  RESP", [&] { (void)resp->command(hset10); });
  measure("  SBE", [&] { (void)sbe->hset("h", sbe_h10); });

  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);
  ::unlink(resp_ring.c_str());
  ::unlink(sbe_ring.c_str());
  ::unlink(sock.c_str());
  return 0;
}
