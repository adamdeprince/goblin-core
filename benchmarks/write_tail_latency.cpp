// Write-path tail-latency probe, C++ client (see BENCHMARKS.md).
//
// Low-overhead counterpart to write_tail_latency.py: one connection, no
// pipelining, each ZADD timed individually while a set grows from empty. A
// tight C++ client removes the interpreter overhead so the p50-p99.9 band
// reflects the true round trip, not the client. Build and run:
//
//   c++ -O2 -std=c++20 -o write_tail_latency write_tail_latency.cpp
//   taskset -c 1 ./write_tail_latency 127.0.0.1 6379 1000000
//
// Pin the server and this client to separate cores for clean numbers.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
  const char* host = argc > 1 ? argv[1] : "127.0.0.1";
  const int port = argc > 2 ? std::atoi(argv[2]) : 6379;
  const long n = argc > 3 ? std::atol(argv[3]) : 1000000;

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, host, &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("connect");
    return 1;
  }

  std::vector<double> lat(static_cast<size_t>(n));
  char cmd[128];
  char rbuf[64];
  using clk = std::chrono::steady_clock;

  for (long i = 0; i < n; ++i) {
    char sc[16];
    const int sclen = std::snprintf(sc, sizeof(sc), "%ld", i % 1000);
    char mem[32];
    const int mlen = std::snprintf(mem, sizeof(mem), "m:%ld", i);
    const int len = std::snprintf(
        cmd, sizeof(cmd),
        "*4\r\n$4\r\nZADD\r\n$4\r\ntail\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n",
        sclen, sc, mlen, mem);

    const auto t0 = clk::now();
    if (::write(fd, cmd, static_cast<size_t>(len)) != len) {
      std::fprintf(stderr, "write failed at op %ld\n", i);
      return 1;
    }
    int got = 0;
    for (;;) {
      const ssize_t r = ::read(fd, rbuf + got, sizeof(rbuf) - static_cast<size_t>(got));
      if (r <= 0) {
        std::fprintf(stderr, "connection closed at op %ld\n", i);
        return 1;
      }
      got += static_cast<int>(r);
      if (got >= 2 && rbuf[got - 1] == '\n') break;
    }
    const auto t1 = clk::now();
    lat[static_cast<size_t>(i)] =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
  }

  std::sort(lat.begin(), lat.end());
  auto pct = [&](double p) {
    return lat[std::min(static_cast<long>(static_cast<double>(n) * p), n - 1)];
  };
  std::printf(
      "n=%ld p50=%.2f p90=%.2f p99=%.2f p99.9=%.2f p99.99=%.2f max=%.2f us\n", n,
      pct(0.5), pct(0.9), pct(0.99), pct(0.999), pct(0.9999), lat[static_cast<size_t>(n - 1)]);
  long s1 = 0, s5 = 0;
  for (double x : lat) {
    if (x > 1000) ++s1;
    if (x > 5000) ++s5;
  }
  std::printf("ops>1ms=%ld ops>5ms=%ld\n", s1, s5);
  return 0;
}
