// Unpipelined single-op round-trip latency: Goblin Core's SBE-over-shared-memory-ring
// against RESP-over-Unix-socket on Goblin itself and on the established Redis-family
// incumbents. A TCP mode exists for engines that do not yet expose a Unix socket.
//
// One C++ client and one rdtscp timing method drive every engine, so the only variable
// is (transport, protocol, server) -- not the client language. Each op is a synchronous
// send-one-command / read-one-reply round trip in a tight loop; we report the median (the
// figure to read) plus p90/p99/min over a fixed window.
//
// Low cardinality is deliberate. The zset holds 10 members and the hash 10 fields, so at
// these thresholds the configured established engines are on their compact small-collection
// paths and there is almost no data-structure work per op. What remains is wire-path overhead
// -- which is exactly the combined SBE/ring story.
//
//   latency_shootout ring <goblin-core> [label]   # Goblin SBE over the shared-memory ring
//   latency_shootout uds  <socket> <label>        # RESP over a Unix socket (any engine)
//   latency_shootout tcp  <host> <port> <label>   # RESP over TCP (any engine)
//
// Emits one CSV line per op on stdout for the driver to aggregate:
//   LAT,<label>,<op>,<p50us>,<p90us>,<p99us>,<minus>,<meanus>,<n>
// and a human-readable line per op on stderr for live monitoring.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
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

namespace {

// Pin cores, overridable via SERVER_CORE / CLIENT_CORE env vars -- so a NUMA run can
// put the client on a core local to (or remote from) the server's ring node.
inline int env_core(const char* name, int dflt) {
  const char* e = std::getenv(name);
  return e != nullptr ? std::atoi(e) : dflt;
}
const int kServerCore = env_core("SERVER_CORE", 2);  // busy-poll server pinned here
const int kClientCore = env_core("CLIENT_CORE", 3);  // measuring thread pinned here
constexpr double kWindowSec = 2.0;
constexpr double kWarmupSec = 0.3;
constexpr int kCard = 10;        // cardinality of the zset / hash under test

clk::time_point after(double s) {
  return clk::now() + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(s));
}

// Per-op timing reads the CPU cycle counter, not clock_gettime: rdtscp on x86 is ~7 ns and
// cycle-resolution (invariant TSC counts at a constant rate regardless of boost clock),
// versus ~25 ns of call overhead from steady_clock. arm64 reads the generic timer. Calibrated
// once against steady_clock on x86.
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

void pin_to(int core) {
#ifdef __linux__
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  (void)::sched_setaffinity(0, sizeof(set), &set);
#else
  (void)core;
#endif
}

template <class Op>
void measure(const char* label, const char* op, Op&& fn) {
  for (auto w = after(kWarmupSec); clk::now() < w;) fn();
  std::vector<long long> ns;
  ns.reserve(4'000'000);
  for (auto end = after(kWindowSec); clk::now() < end;) {
    const std::uint64_t a = hw_ticks();
    fn();
    const std::uint64_t b = hw_ticks();
    ns.push_back(static_cast<long long>(static_cast<double>(b - a) * g_ns_per_tick));
  }
  std::sort(ns.begin(), ns.end());
  const std::size_t n = ns.size();
  const double p50 = ns[n / 2] / 1000.0;
  const double p90 = ns[std::size_t(n * 0.90)] / 1000.0;
  const double p99 = ns[std::size_t(n * 0.99)] / 1000.0;
  const double mn = ns[0] / 1000.0;
  const double mean = std::accumulate(ns.begin(), ns.end(), 0.0) / n / 1000.0;
  // Machine-readable for the driver.
  std::printf("LAT,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n", label, op, p50, p90, p99, mn, mean, n);
  std::fflush(stdout);
  // Human-readable for live monitoring.
  std::fprintf(stderr, "  %-8s %-7s p50=%7.3f  p90=%7.3f  p99=%7.3f  min=%7.3f us  (n=%zu)\n",
               label, op, p50, p90, p99, mn, n);
}

// ---- minimal synchronous RESP client over a connected fd ----------------------------------
// Sends one RESP array command, reads exactly one reply (any RESP2/RESP3 type), returns the
// reply's leading type byte so callers can spot an error ('-') during setup. No pipelining.
class RespConn {
 public:
  static RespConn connect_uds(const char* path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      throw std::runtime_error(std::string("connect ") + path);
    }
    return RespConn(fd);
  }

  static RespConn connect_tcp(const char* host, int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket");
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      throw std::runtime_error(std::string("connect ") + host + ":" +
                               std::to_string(port));
    }
    return RespConn(fd);
  }
  ~RespConn() { if (fd_ >= 0) ::close(fd_); }
  RespConn(RespConn&& o) noexcept : fd_(o.fd_), buf_(std::move(o.buf_)) { o.fd_ = -1; }
  RespConn& operator=(RespConn&&) = delete;

  char command(std::span<const std::string_view> args) {
    send_cmd(args);
    const std::size_t end = parse(0);
    const char type = buf_[0];
    buf_.erase(0, end);
    return type;
  }

 private:
  explicit RespConn(int fd) : fd_(fd) { buf_.reserve(8192); }

  void send_cmd(std::span<const std::string_view> args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto a : args) {
      out += '$';
      out += std::to_string(a.size());
      out += "\r\n";
      out.append(a.data(), a.size());
      out += "\r\n";
    }
    std::size_t off = 0;
    while (off < out.size()) {
      const ssize_t w = ::send(fd_, out.data() + off, out.size() - off, 0);
      if (w <= 0) throw std::runtime_error("send");
      off += static_cast<std::size_t>(w);
    }
  }
  void fill() {
    char tmp[8192];
    const ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
    if (r <= 0) throw std::runtime_error("recv (server closed?)");
    buf_.append(tmp, static_cast<std::size_t>(r));
  }
  // Returns the offset just past a single complete reply starting at `pos`, reading more
  // bytes from the socket as needed.
  std::size_t parse(std::size_t pos) {
    while (buf_.size() <= pos) fill();
    const char type = buf_[pos];
    std::size_t eol;
    while ((eol = buf_.find("\r\n", pos)) == std::string::npos) fill();
    const std::size_t after_line = eol + 2;
    if (type == '$') {  // bulk string: $<len>\r\n<len bytes>\r\n  (len < 0 => nil, no body)
      const long len = std::strtol(buf_.c_str() + pos + 1, nullptr, 10);
      if (len < 0) return after_line;
      const std::size_t need = after_line + static_cast<std::size_t>(len) + 2;
      while (buf_.size() < need) fill();
      return need;
    }
    if (type == '*' || type == '~' || type == '>') {  // array/set/push: parse N elements
      const long count = std::strtol(buf_.c_str() + pos + 1, nullptr, 10);
      std::size_t cur = after_line;
      for (long i = 0; i < count; ++i) cur = parse(cur);
      return cur;
    }
    if (type == '%') {  // map: 2*N elements
      const long count = std::strtol(buf_.c_str() + pos + 1, nullptr, 10);
      std::size_t cur = after_line;
      for (long i = 0; i < count * 2; ++i) cur = parse(cur);
      return cur;
    }
    // Single-line types: + - : , # _ (  -> the reply ends at the CRLF.
    return after_line;
  }

  int fd_;
  std::string buf_;
};

// Shared small fixtures: 10 members / fields / values, one target element in the middle.
const char* kMem[kCard] = {"m0", "m1", "m2", "m3", "m4", "m5", "m6", "m7", "m8", "m9"};
const char* kField[kCard] = {"f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9"};
const char* kVal[kCard] = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9"};
const char* kScore[kCard] = {"1.1", "2.2", "3.3", "4.4", "5.5", "6.6", "7.7", "8.8", "9.9", "10.1"};
constexpr const char* kZKey = "z";
constexpr const char* kHKey = "h";
constexpr const char* kSKey = "s";
constexpr const char* kTarget = "5";  // suffix: measured element is m5 / f5 (a middle one)

// ---- Goblin SBE over the shared-memory ring -----------------------------------------------
int run_ring(const char* server, const char* label) {
  const std::string tag = std::to_string(::getpid());
  const std::string ring = "/tmp/gclat-sbe-" + tag + ".ring";
  const std::string sock = "/tmp/gclat-" + tag + ".sock";
  ::unlink(ring.c_str());

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
#ifdef __linux__
    const std::string core = std::to_string(kServerCore);
    ::execl("/usr/bin/taskset", "taskset", "-c", core.c_str(), server,
            "--enable-sbe", "--unixsocket",
            sock.c_str(), "--ring", ring.c_str(), "1mb", static_cast<char*>(nullptr));
#endif
    ::execl(server, server, "--enable-sbe", "--unixsocket", sock.c_str(),
            "--ring", ring.c_str(), "1mb", static_cast<char*>(nullptr));
    _exit(127);
  }

  pin_to(kClientCore);
  goblin::core::ring::set_busy_poll_thread_realtime();  // no-op off macOS
  calibrate_hw_ticks();

  auto sbe = SbeRingClient::open(ring.c_str(), std::chrono::seconds(5));
  if (!sbe) { std::fprintf(stderr, "ring: could not open %s\n", ring.c_str()); ::kill(pid, SIGTERM); return 1; }

  // Populate: a 10-member zset, a 10-field hash, one string. Keeps cardinality at 10.
  std::vector<SbeRingClient::Scored> members;
  std::vector<std::pair<std::string_view, std::string_view>> fields;
  for (int i = 0; i < kCard; ++i) {
    members.push_back({1.1 * (i + 1), kMem[i]});
    fields.push_back({kField[i], kVal[i]});
  }
  (void)sbe->zadd(kZKey, members);
  (void)sbe->hset(kHKey, fields);
  (void)sbe->set(kSKey, "v");

  const std::string zmem = std::string("m") + kTarget;
  const std::string hfield = std::string("f") + kTarget;
  const std::vector<std::pair<std::string_view, std::string_view>> one_field = {{hfield, "v5"}};
  const std::vector<SbeRingClient::Scored> one_member = {{5.5, zmem}};

  std::fprintf(stderr, "[%s] SBE / shared-memory ring (zset=%d, hash=%d):\n", label, kCard, kCard);
  measure(label, "SET",    [&] { (void)sbe->set(kSKey, "v"); });
  measure(label, "GET",    [&] { (void)sbe->get(kSKey); });
  measure(label, "HSET",   [&] { (void)sbe->hset(kHKey, one_field); });
  measure(label, "HGET",   [&] { (void)sbe->hget(kHKey, hfield); });
  measure(label, "ZADD",   [&] { (void)sbe->zadd(kZKey, one_member); });
  measure(label, "ZSCORE", [&] { (void)sbe->zscore(kZKey, zmem); });

  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);
  ::unlink(ring.c_str());
  ::unlink(sock.c_str());
  return 0;
}

// ---- RESP over a connected Unix or TCP socket ---------------------------------------------
int run_resp(RespConn conn, const char* label, const char* transport,
             bool allow_unsupported) {
  pin_to(kClientCore);
  calibrate_hw_ticks();

  // Populate: ZADD 10 members, HSET 10 fields, SET one string.
  std::vector<std::string_view> zadd = {"ZADD", kZKey};
  std::vector<std::string_view> hset = {"HSET", kHKey};
  for (int i = 0; i < kCard; ++i) {
    zadd.push_back(kScore[i]);
    zadd.push_back(kMem[i]);
    hset.push_back(kField[i]);
    hset.push_back(kVal[i]);
  }
  const std::vector<std::string_view> setcmd = {"SET", kSKey, "v"};
  const bool strings_supported = conn.command(setcmd) != '-';
  const bool hashes_supported = conn.command(hset) != '-';
  const bool zsets_supported = conn.command(zadd) != '-';
  if (!allow_unsupported &&
      (!strings_supported || !hashes_supported || !zsets_supported)) {
    std::fprintf(stderr, "%s: setup command errored on %s\n", transport, label);
    return 1;
  }

  const std::string zmem = std::string("m") + kTarget;
  const std::string hfield = std::string("f") + kTarget;
  const std::vector<std::string_view> c_set    = {"SET", kSKey, "v"};
  const std::vector<std::string_view> c_get    = {"GET", kSKey};
  const std::vector<std::string_view> c_hset   = {"HSET", kHKey, hfield, "v5"};
  const std::vector<std::string_view> c_hget   = {"HGET", kHKey, hfield};
  const std::vector<std::string_view> c_zadd   = {"ZADD", kZKey, "5.5", zmem};
  const std::vector<std::string_view> c_zscore = {"ZSCORE", kZKey, zmem};

  std::fprintf(stderr, "[%s] RESP / %s (zset=%d, hash=%d):\n", label,
               transport, kCard, kCard);
  if (strings_supported) {
    measure(label, "SET", [&] { (void)conn.command(c_set); });
    measure(label, "GET", [&] { (void)conn.command(c_get); });
  }
  if (hashes_supported) {
    measure(label, "HSET", [&] { (void)conn.command(c_hset); });
    measure(label, "HGET", [&] { (void)conn.command(c_hget); });
  }
  if (zsets_supported) {
    measure(label, "ZADD", [&] { (void)conn.command(c_zadd); });
    measure(label, "ZSCORE", [&] { (void)conn.command(c_zscore); });
  }
  return 0;
}

int run_uds(const char* socket_path, const char* label) {
  return run_resp(RespConn::connect_uds(socket_path), label, "Unix socket", false);
}

int run_tcp(const char* host, int port, const char* label) {
  return run_resp(RespConn::connect_tcp(host, port), label, "TCP", true);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "ring") == 0)
    return run_ring(argv[2], argc >= 4 ? argv[3] : "goblin-sbe-ring");
  if (argc >= 4 && std::strcmp(argv[1], "uds") == 0)
    return run_uds(argv[2], argv[3]);
  if (argc >= 5 && std::strcmp(argv[1], "tcp") == 0)
    return run_tcp(argv[2], std::atoi(argv[3]), argv[4]);
  std::fprintf(stderr,
               "usage:\n"
               "  latency_shootout ring <goblin-core> [label]\n"
               "  latency_shootout uds  <socket> <label>\n"
               "  latency_shootout tcp  <host> <port> <label>\n");
  return 2;
}
