// Multi-threaded ZADD load generator (C++).
//
// One OS thread per connection, each driving synchronous ZADDs (one per round
// trip) unless --pipeline > 1. Unlike redis-benchmark's single event-loop
// thread, this scales the *client* across cores, so a single-threaded server's
// true one-core ceiling is what's measured, not the load generator.
//
//   zadd_bench --unixsocket /path --threads 32 --duration 5
//   zadd_bench --host 127.0.0.1 --port 6379 --threads 32 --pipeline 1
//
// Prints one line: aggregate ZADD/sec. Key/score/member are each a random int in
// [0,keyspace) (matching redis-benchmark's __rand_int__ with -r keyspace).

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct Config {
  std::string host = "127.0.0.1";
  int port = 6379;
  std::string unix_path;
  int threads = 8;
  int keyspace = 200000;
  int pipeline = 1;
  double duration = 5.0;
};

int connect_socket(const Config& c) {
  int fd = -1;
  if (!c.unix_path.empty()) {
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, c.unix_path.c_str(), sizeof(a.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      ::close(fd);
      return -1;
    }
  } else {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(c.port));
    ::inet_pton(AF_INET, c.host.c_str(), &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      ::close(fd);
      return -1;
    }
  }
  return fd;
}

void append_bulk(std::string& o, const char* s, int n) {
  char b[24];
  auto r = std::to_chars(b, b + sizeof(b), n);
  o += '$';
  o.append(b, r.ptr - b);
  o += "\r\n";
  o.append(s, static_cast<std::size_t>(n));
  o += "\r\n";
}

bool send_all(int fd, const char* p, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    ssize_t w = ::send(fd, p + off, n - off, 0);
    if (w <= 0) return false;
    off += static_cast<std::size_t>(w);
  }
  return true;
}

// ZADD replies are single-line (":N\r\n"), so one '\n' terminates each -- read
// until `count` of them are seen.
bool read_replies(int fd, int count, char* buf, std::size_t bufsz) {
  int seen = 0;
  while (seen < count) {
    ssize_t n = ::recv(fd, buf, bufsz, 0);
    if (n <= 0) return false;
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') ++seen;
    }
  }
  return true;
}

std::atomic<bool> g_stop{false};
std::atomic<long> g_done{0};
std::atomic<int> g_errors{0};

void worker(Config c, unsigned seed) {
  int fd = connect_socket(c);
  if (fd < 0) {
    ++g_errors;
    return;
  }
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::uint64_t> dist(
      0, static_cast<std::uint64_t>(c.keyspace) - 1);
  std::string batch;
  std::vector<char> rbuf(65536);
  long done = 0;
  while (!g_stop.load(std::memory_order_relaxed)) {
    batch.clear();
    for (int i = 0; i < c.pipeline; ++i) {
      char key[24], sc[24], mem[24];
      int kl = std::snprintf(key, sizeof key, "z:%llu",
                             static_cast<unsigned long long>(dist(rng)));
      int sl = std::snprintf(sc, sizeof sc, "%llu",
                             static_cast<unsigned long long>(dist(rng)));
      int ml = std::snprintf(mem, sizeof mem, "m:%llu",
                             static_cast<unsigned long long>(dist(rng)));
      batch += "*4\r\n$4\r\nZADD\r\n";
      append_bulk(batch, key, kl);
      append_bulk(batch, sc, sl);
      append_bulk(batch, mem, ml);
    }
    if (!send_all(fd, batch.data(), batch.size())) {
      ++g_errors;
      break;
    }
    if (!read_replies(fd, c.pipeline, rbuf.data(), rbuf.size())) {
      ++g_errors;
      break;
    }
    done += c.pipeline;
  }
  g_done += done;
  ::close(fd);
}

std::string next_arg(int argc, char** argv, int& i) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "missing value for %s\n", argv[i]);
    std::exit(2);
  }
  return argv[++i];
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--host") cfg.host = next_arg(argc, argv, i);
    else if (a == "--port") cfg.port = std::stoi(next_arg(argc, argv, i));
    else if (a == "--unixsocket") cfg.unix_path = next_arg(argc, argv, i);
    else if (a == "--threads") cfg.threads = std::stoi(next_arg(argc, argv, i));
    else if (a == "--keyspace") cfg.keyspace = std::stoi(next_arg(argc, argv, i));
    else if (a == "--pipeline") cfg.pipeline = std::stoi(next_arg(argc, argv, i));
    else if (a == "--duration") cfg.duration = std::stod(next_arg(argc, argv, i));
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
  }

  std::vector<std::thread> pool;
  pool.reserve(cfg.threads);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < cfg.threads; ++i) {
    pool.emplace_back(worker, cfg, 0x9e3779b97f4a7c15ULL * (i + 1));
  }
  std::this_thread::sleep_for(
      std::chrono::duration<double>(cfg.duration));
  g_stop.store(true, std::memory_order_relaxed);
  for (auto& t : pool) t.join();
  auto t1 = std::chrono::steady_clock::now();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  long total = g_done.load();
  if (g_errors.load() > 0) {
    std::fprintf(stderr, "zadd_bench: %d connection/io errors\n",
                 g_errors.load());
  }
  std::printf("%.0f\n", static_cast<double>(total) / secs);
  return total > 0 ? 0 : 1;
}
