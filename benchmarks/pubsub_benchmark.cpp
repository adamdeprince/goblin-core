// Native end-to-end Pub/Sub benchmark.
//
// Goblin is exercised through both typed SBE/shared-memory rings and RESP2/UDS.
// Incumbents are black-box RESP2/UDS servers. The same process, RESP parser,
// cross-core timestamp source, workload definitions, and validation drive every
// row. Python and redis-cli are deliberately absent from the measured path.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifdef __linux__
#include <sched.h>
#endif

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using goblin::core::PubSubKind;
using goblin::core::SbeRingClient;

namespace {

constexpr std::string_view kLiteralChannel = "bench:pubsub:literal";
constexpr std::string_view kPatternMissChannel = "bench:pubsub:unmatched";

double g_ns_per_tick = 1.0;

[[gnu::always_inline]] inline std::uint64_t hardware_ticks() noexcept {
#if defined(__x86_64__)
  unsigned aux = 0;
  return __rdtscp(&aux);
#elif defined(__aarch64__)
  std::uint64_t value = 0;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
  return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

void calibrate_ticks() {
#if defined(__aarch64__)
  std::uint64_t frequency = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  g_ns_per_tick = 1e9 / static_cast<double>(frequency);
#elif defined(__x86_64__)
  const auto wall_start = Clock::now();
  const auto tick_start = hardware_ticks();
  while (Clock::now() - wall_start < std::chrono::milliseconds(250)) {
  }
  const auto tick_end = hardware_ticks();
  const auto wall_end = Clock::now();
  g_ns_per_tick =
      std::chrono::duration<double, std::nano>(wall_end - wall_start).count() /
      static_cast<double>(tick_end - tick_start);
#endif
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

[[nodiscard]] int online_cpus() noexcept {
  const long value = ::sysconf(_SC_NPROCESSORS_ONLN);
  return value > 0 ? static_cast<int>(value) : 1;
}

[[nodiscard]] int listener_cpu(int first, std::size_t index) noexcept {
  const int cpus = online_cpus();
  if (first < 0 || cpus <= 1) {
    return -1;
  }
  int candidate = first + static_cast<int>(index);
  // On naamah CPUs 0-63 are the first hardware threads and 64-127 their SMT
  // siblings. After using physical cores 4-63, continue at 68 so listeners do
  // not share cores 2/3 with the server and publisher.
  if (cpus >= 2 * (first + 32) && candidate >= cpus / 2) {
    candidate += first;
  }
  return candidate < cpus ? candidate : candidate % cpus;
}

[[nodiscard]] std::string errno_message(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

void send_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      bytes.remove_prefix(static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error(errno_message("send"));
  }
}

[[nodiscard]] std::string encode_resp_command(
    std::span<const std::string_view> args) {
  std::size_t bytes = 16;
  for (const auto arg : args) {
    bytes += arg.size() + 32;
  }
  std::string out;
  out.reserve(bytes);
  out.push_back('*');
  out += std::to_string(args.size());
  out += "\r\n";
  for (const auto arg : args) {
    out.push_back('$');
    out += std::to_string(arg.size());
    out += "\r\n";
    out.append(arg.data(), arg.size());
    out += "\r\n";
  }
  return out;
}

struct RespValue {
  char type{'_'};
  long long integer{0};
  std::string text;
  std::vector<RespValue> elements;
};

struct Delivery {
  bool pattern{false};
  std::string pattern_name;
  std::string channel;
  std::string payload;
};

class RespConnection {
 public:
  static RespConnection connect_uds(std::string_view path,
                                    std::chrono::milliseconds timeout =
                                        std::chrono::seconds(5)) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
      const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) {
        throw std::runtime_error(errno_message("socket"));
      }
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        throw std::runtime_error("Unix socket path is too long");
      }
      std::memcpy(address.sun_path, path.data(), path.size());
      address.sun_path[path.size()] = '\0';
      if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
          0) {
        return RespConnection(fd);
      }
      const int error = errno;
      ::close(fd);
      if (Clock::now() >= deadline) {
        errno = error;
        throw std::runtime_error(errno_message("connect " + std::string(path)));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ~RespConnection() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  RespConnection(const RespConnection&) = delete;
  RespConnection& operator=(const RespConnection&) = delete;

  RespConnection(RespConnection&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        input_(std::move(other.input_)),
        input_offset_(other.input_offset_) {}

  RespConnection& operator=(RespConnection&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        ::close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
      input_ = std::move(other.input_);
      input_offset_ = other.input_offset_;
    }
    return *this;
  }

  [[nodiscard]] RespValue request(std::span<const std::string_view> args) {
    const auto command = encode_resp_command(args);
    return request_encoded(command);
  }

  [[nodiscard]] RespValue request_encoded(std::string_view command) {
    send_all(fd_, command);
    return read_value();
  }

  void ping() {
    static constexpr std::string_view args[] = {"PING"};
    const auto reply = request(args);
    if (reply.type != '+' || reply.text != "PONG") {
      throw std::runtime_error("server did not answer PING with PONG");
    }
  }

  void subscribe(const std::vector<std::string>& channels) {
    subscription_command("SUBSCRIBE", channels);
  }

  void psubscribe(const std::vector<std::string>& patterns) {
    subscription_command("PSUBSCRIBE", patterns);
  }

  [[nodiscard]] long long publish(std::string_view channel,
                                  std::string_view payload) {
    if (cached_publish_channel_ != channel || cached_publish_payload_ != payload) {
      const std::string_view args[] = {"PUBLISH", channel, payload};
      cached_publish_ = encode_resp_command(args);
      cached_publish_channel_.assign(channel);
      cached_publish_payload_.assign(payload);
    }
    const auto reply = request_encoded(cached_publish_);
    if (reply.type == '-') {
      throw std::runtime_error("PUBLISH failed: " + reply.text);
    }
    if (reply.type != ':') {
      throw std::runtime_error("PUBLISH returned a non-integer RESP reply");
    }
    return reply.integer;
  }

  [[nodiscard]] Delivery read_pubsub() {
    const auto reply = read_value();
    if (reply.type == '-') {
      throw std::runtime_error("Pub/Sub stream error: " + reply.text);
    }
    if ((reply.type != '*' && reply.type != '>') || reply.elements.empty()) {
      throw std::runtime_error("unexpected Pub/Sub delivery shape");
    }
    const auto& kind = reply.elements[0].text;
    if (kind == "message" && reply.elements.size() == 3) {
      return Delivery{false, {}, reply.elements[1].text, reply.elements[2].text};
    }
    if (kind == "pmessage" && reply.elements.size() == 4) {
      return Delivery{true, reply.elements[1].text, reply.elements[2].text,
                      reply.elements[3].text};
    }
    throw std::runtime_error("unexpected Pub/Sub message kind: " + kind);
  }

 private:
  explicit RespConnection(int fd) : fd_(fd) { input_.reserve(64 * 1024); }

  void subscription_command(std::string_view command,
                            const std::vector<std::string>& names) {
    if (names.empty()) {
      throw std::runtime_error("subscription command requires at least one name");
    }
    std::vector<std::string_view> args;
    args.reserve(names.size() + 1);
    args.push_back(command);
    for (const auto& name : names) {
      args.push_back(name);
    }
    send_all(fd_, encode_resp_command(args));
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto ack = read_value();
      if (ack.type == '-') {
        throw std::runtime_error(std::string(command) + " failed: " + ack.text);
      }
      if ((ack.type != '*' && ack.type != '>') || ack.elements.size() != 3) {
        throw std::runtime_error(std::string(command) + " returned an invalid ack");
      }
    }
  }

  void fill() {
    char buffer[64 * 1024];
    for (;;) {
      const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
      if (received > 0) {
        input_.append(buffer, static_cast<std::size_t>(received));
        return;
      }
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received == 0) {
        throw std::runtime_error("server closed the Pub/Sub connection");
      }
      throw std::runtime_error(errno_message("recv"));
    }
  }

  void ensure(std::size_t bytes) {
    while (input_.size() < bytes) {
      fill();
    }
  }

  [[nodiscard]] std::string_view line(std::size_t& cursor) {
    for (;;) {
      const auto end = input_.find("\r\n", cursor);
      if (end != std::string::npos) {
        const auto begin = cursor;
        cursor = end + 2;
        return std::string_view(input_).substr(begin, end - begin);
      }
      fill();
    }
  }

  [[nodiscard]] static long long parse_integer(std::string_view text) {
    long long value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
      throw std::runtime_error("invalid RESP integer");
    }
    return value;
  }

  [[nodiscard]] RespValue parse_value(std::size_t& cursor) {
    ensure(cursor + 1);
    RespValue value;
    value.type = input_[cursor++];
    const auto header = line(cursor);
    switch (value.type) {
      case '+':
      case '-':
      case ',':
      case '(':
      case '#':
        value.text.assign(header);
        return value;
      case ':':
        value.integer = parse_integer(header);
        return value;
      case '_':
        return value;
      case '$': {
        const auto length = parse_integer(header);
        if (length < 0) {
          return value;
        }
        const auto size = static_cast<std::size_t>(length);
        ensure(cursor + size + 2);
        value.text.assign(input_.data() + cursor, size);
        cursor += size;
        if (input_.compare(cursor, 2, "\r\n") != 0) {
          throw std::runtime_error("invalid RESP bulk terminator");
        }
        cursor += 2;
        return value;
      }
      case '*':
      case '>':
      case '~':
      case '%': {
        const auto count = parse_integer(header);
        if (count < 0) {
          return value;
        }
        const auto multiplier = value.type == '%' ? 2LL : 1LL;
        const auto elements = count * multiplier;
        value.elements.reserve(static_cast<std::size_t>(elements));
        for (long long i = 0; i < elements; ++i) {
          value.elements.push_back(parse_value(cursor));
        }
        return value;
      }
      default:
        throw std::runtime_error("unsupported RESP reply type");
    }
  }

  [[nodiscard]] RespValue read_value() {
    std::size_t cursor = input_offset_;
    auto value = parse_value(cursor);
    input_offset_ = cursor;
    if (input_offset_ == input_.size()) {
      input_.clear();
      input_offset_ = 0;
    } else if (input_offset_ >= 1024 * 1024) {
      input_.erase(0, input_offset_);
      input_offset_ = 0;
    }
    return value;
  }

  int fd_{-1};
  std::string input_;
  std::size_t input_offset_{0};
  std::string cached_publish_;
  std::string cached_publish_channel_;
  std::string cached_publish_payload_;
};

class RingConnection {
 public:
  static RingConnection open(std::string_view path) {
    auto client = SbeRingClient::open(std::string(path).c_str(),
                                      std::chrono::seconds(10));
    if (!client) {
      throw std::runtime_error("could not open SBE ring " + std::string(path));
    }
    return RingConnection(std::move(*client));
  }

  void ping() {
    if (!client_.ping()) {
      throw std::runtime_error("SBE ring did not answer PING");
    }
  }

  void subscribe(const std::vector<std::string>& channels) {
    const auto views = as_views(channels);
    const auto acks = client_.subscribe(views, std::chrono::seconds(30));
    if (acks.size() != channels.size()) {
      throw std::runtime_error("SBE SUBSCRIBE returned the wrong ack count");
    }
  }

  void psubscribe(const std::vector<std::string>& patterns) {
    const auto views = as_views(patterns);
    const auto acks = client_.psubscribe(views, std::chrono::seconds(30));
    if (acks.size() != patterns.size()) {
      throw std::runtime_error("SBE PSUBSCRIBE returned the wrong ack count");
    }
  }

  [[nodiscard]] long long publish(std::string_view channel,
                                  std::string_view payload) {
    return client_.publish(channel, payload, std::chrono::seconds(30));
  }

  [[nodiscard]] Delivery read_pubsub() {
    auto message = client_.read_pubsub(std::chrono::seconds(30));
    if (message.kind == PubSubKind::message) {
      return Delivery{false, {}, std::move(message.channel),
                      std::move(message.payload)};
    }
    if (message.kind == PubSubKind::pattern_message) {
      return Delivery{true, std::move(message.pattern), std::move(message.channel),
                      std::move(message.payload)};
    }
    throw std::runtime_error("unexpected SBE Pub/Sub acknowledgement while reading");
  }

 private:
  explicit RingConnection(SbeRingClient&& client) : client_(std::move(client)) {}

  [[nodiscard]] static std::vector<std::string_view> as_views(
      const std::vector<std::string>& strings) {
    std::vector<std::string_view> views;
    views.reserve(strings.size());
    for (const auto& value : strings) {
      views.push_back(value);
    }
    return views;
  }

  SbeRingClient client_;
};

enum class EngineKind { goblin_ring, goblin_uds, redis, dragonfly, mini };

struct EngineSpec {
  std::string label;
  EngineKind kind{EngineKind::redis};
  fs::path binary;

  [[nodiscard]] bool uses_ring() const noexcept {
    return kind == EngineKind::goblin_ring;
  }
};

struct RunConfig {
  std::vector<EngineSpec> engines;
  fs::path parity_config = fs::path(__FILE__).parent_path() / "redis-parity.conf";
  int server_cpu{2};
  int publisher_cpu{3};
  int first_listener_cpu{4};
  bool smoke{false};
  std::size_t latency_samples{100'000};
  std::size_t latency_warmup{10'000};
  std::size_t fanout_samples{20'000};
  std::size_t fanout_warmup{2'000};
  std::size_t routing_samples{20'000};
  std::size_t routing_warmup{2'000};
};

[[nodiscard]] std::string kind_name(EngineKind kind) {
  switch (kind) {
    case EngineKind::goblin_ring:
      return "goblin-ring";
    case EngineKind::goblin_uds:
      return "goblin-uds";
    case EngineKind::redis:
      return "redis";
    case EngineKind::dragonfly:
      return "dragonfly";
    case EngineKind::mini:
      return "mini";
  }
  throw std::runtime_error("unknown engine kind");
}

[[nodiscard]] std::optional<EngineKind> parse_kind(std::string_view text) {
  if (text == "goblin-ring") {
    return EngineKind::goblin_ring;
  }
  if (text == "goblin-uds") {
    return EngineKind::goblin_uds;
  }
  if (text == "redis") {
    return EngineKind::redis;
  }
  if (text == "dragonfly") {
    return EngineKind::dragonfly;
  }
  if (text == "mini") {
    return EngineKind::mini;
  }
  return std::nullopt;
}

[[nodiscard]] EngineSpec parse_engine(std::string_view text) {
  const auto first = text.find(':');
  const auto second = first == std::string_view::npos
                          ? std::string_view::npos
                          : text.find(':', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      first == 0 || second == first + 1 || second + 1 == text.size()) {
    throw std::runtime_error(
        "--engine must be LABEL:KIND:PATH (PATH may contain additional colons)");
  }
  const auto kind = parse_kind(text.substr(first + 1, second - first - 1));
  if (!kind) {
    throw std::runtime_error("unknown engine kind in " + std::string(text));
  }
  return EngineSpec{std::string(text.substr(0, first)), *kind,
                    fs::path(text.substr(second + 1))};
}

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

[[nodiscard]] std::uint64_t read_resident_bytes(pid_t pid, bool use_ps) {
#ifdef __linux__
  if (use_ps) {
    int output[2];
    if (::pipe(output) != 0) {
      throw std::runtime_error(errno_message("pipe"));
    }
    const pid_t reader = ::fork();
    if (reader < 0) {
      const auto error = errno_message("fork");
      ::close(output[0]);
      ::close(output[1]);
      throw std::runtime_error(error);
    }
    if (reader == 0) {
      (void)::dup2(output[1], STDOUT_FILENO);
      ::close(output[0]);
      ::close(output[1]);
      const auto text_pid = std::to_string(pid);
      ::execl("/bin/ps", "ps", "-o", "rss=", "-p", text_pid.c_str(),
              static_cast<char*>(nullptr));
      _exit(127);
    }
    ::close(output[1]);
    std::string text;
    char buffer[128];
    for (;;) {
      const auto bytes = ::read(output[0], buffer, sizeof(buffer));
      if (bytes > 0) {
        text.append(buffer, static_cast<std::size_t>(bytes));
        continue;
      }
      if (bytes < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    ::close(output[0]);
    int status = 0;
    if (::waitpid(reader, &status, 0) != reader || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
      throw std::runtime_error("ps failed while reading process RSS");
    }
    std::uint64_t kib = 0;
    std::istringstream input(text);
    if (!(input >> kib)) {
      throw std::runtime_error("ps returned no process RSS");
    }
    return kib * 1024;
  }
  std::ifstream status("/proc/" + std::to_string(pid) + "/status");
  std::string line;
  std::uint64_t kib = 0;
  while (std::getline(status, line)) {
    if (!line.starts_with("VmRSS:") && !line.starts_with("HugetlbPages:")) {
      continue;
    }
    std::istringstream input(line.substr(line.find(':') + 1));
    std::uint64_t value = 0;
    std::string unit;
    input >> value >> unit;
    if (input && unit == "kB") {
      kib += value;
    }
  }
  return kib * 1024;
#else
  (void)pid;
  (void)use_ps;
  return 0;
#endif
}

[[nodiscard]] std::string read_file(const fs::path& path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

class ServerProcess {
 public:
  ServerProcess(const EngineSpec& engine, const RunConfig& config,
                std::size_t ring_count, std::string_view case_name)
      : engine_(engine) {
    if (!fs::exists(engine.binary)) {
      throw std::runtime_error("server binary does not exist: " +
                               engine.binary.string());
    }
    const auto serial = sequence_.fetch_add(1, std::memory_order_relaxed);
    const auto tag = std::to_string(::getpid()) + "-" + std::to_string(serial);
    socket_path_ = "/tmp/goblin-pubsub-bench-" + tag + ".sock";
    log_path_ = "/tmp/goblin-pubsub-bench-" + tag + ".log";
    (void)::unlink(socket_path_.c_str());

    if (engine.uses_ring()) {
      ring_paths_.reserve(ring_count);
      for (std::size_t i = 0; i < ring_count; ++i) {
        auto path = "/tmp/goblin-pubsub-bench-" + tag + "-" +
                    std::to_string(i) + ".ring";
        (void)::unlink(path.c_str());
        ring_paths_.push_back(std::move(path));
      }
    } else if (ring_count != 0) {
      throw std::runtime_error("ring count supplied for a UDS engine");
    }

    const auto args = command_line(config, case_name, serial);
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error(errno_message("fork"));
    }
    if (pid_ == 0) {
      const int log = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (log >= 0) {
        (void)::dup2(log, STDOUT_FILENO);
        (void)::dup2(log, STDERR_FILENO);
        if (log > STDERR_FILENO) {
          (void)::close(log);
        }
      }
      if (engine.kind == EngineKind::mini) {
        (void)::setenv("GOMAXPROCS", "1", 1);
      }
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(argv.front(), argv.data());
      _exit(127);
    }

    try {
      auto probe = RespConnection::connect_uds(socket_path_, std::chrono::seconds(20));
      probe.ping();
      baseline_rss_ =
          read_resident_bytes(pid_, engine_.kind == EngineKind::mini);
    } catch (const std::exception& error) {
      const auto log = read_file(log_path_);
      stop();
      throw std::runtime_error(std::string(error.what()) + "\nserver log:\n" + log);
    }
  }

  ~ServerProcess() { stop(); }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }
  [[nodiscard]] std::uint64_t baseline_rss() const noexcept { return baseline_rss_; }
  [[nodiscard]] std::uint64_t resident_bytes() const {
    return read_resident_bytes(pid_, engine_.kind == EngineKind::mini);
  }
  [[nodiscard]] const std::string& socket_path() const noexcept {
    return socket_path_;
  }
  [[nodiscard]] const std::vector<std::string>& ring_paths() const noexcept {
    return ring_paths_;
  }

 private:
  [[nodiscard]] std::vector<std::string> command_line(
      const RunConfig& config, std::string_view /*case_name*/,
      std::uint64_t serial) const {
    std::vector<std::string> server;
    switch (engine_.kind) {
      case EngineKind::goblin_ring:
      case EngineKind::goblin_uds: {
        server = {engine_.binary.string(), "--unixsocket", socket_path_,
                  "--unsolicited-output-buffer-bytes", "8192"};
        if (engine_.kind == EngineKind::goblin_ring) {
          server.push_back("--enable-sbe");
        }
        for (const auto& ring : ring_paths_) {
          server.insert(server.end(), {"--ring", ring, "4kb"});
        }
        break;
      }
      case EngineKind::redis:
        server = {engine_.binary.string(), config.parity_config.string(),
                  "--unixsocket", socket_path_, "--port", "0", "--dir", "/tmp"};
        break;
      case EngineKind::dragonfly: {
        const auto port = 20'000 + static_cast<int>((::getpid() + serial) % 20'000);
        server = {engine_.binary.string(), "--unixsocket=" + socket_path_,
                  "--port=" + std::to_string(port), "--proactor_threads=1",
                  "--maxmemory=0", "--dir=/tmp"};
        break;
      }
      case EngineKind::mini:
        server = {engine_.binary.string(), "-unixsocket", socket_path_, "-no-tcp",
                  "-appendonly=false", "-metrics-addr="};
        break;
    }

    if (engine_.kind == EngineKind::dragonfly) {
      return server;
    }
#ifdef __linux__
    std::vector<std::string> pinned = {"/usr/bin/taskset", "-c",
                                       std::to_string(config.server_cpu)};
    pinned.insert(pinned.end(), server.begin(), server.end());
    return pinned;
#else
    (void)config;
    return server;
#endif
  }

  void stop() noexcept {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGTERM);
      for (int i = 0; i < 200; ++i) {
        int status = 0;
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == pid_) {
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
    for (const auto& path : ring_paths_) {
      (void)::unlink(path.c_str());
    }
    if (!log_path_.empty()) {
      (void)::unlink(log_path_.c_str());
    }
  }

  inline static std::atomic<std::uint64_t> sequence_{0};
  EngineSpec engine_;
  pid_t pid_{-1};
  std::string socket_path_;
  std::string log_path_;
  std::vector<std::string> ring_paths_;
  std::uint64_t baseline_rss_{0};
};

struct Distribution {
  double minimum_us{0};
  double p50_us{0};
  double p90_us{0};
  double p99_us{0};
  double p999_us{0};
  double mean_us{0};
};

[[nodiscard]] Distribution summarize_ticks(std::vector<std::uint64_t> ticks) {
  if (ticks.empty()) {
    return {};
  }
  std::sort(ticks.begin(), ticks.end());
  const auto at = [&](double percentile) {
    const auto index = std::min<std::size_t>(
        ticks.size() - 1,
        static_cast<std::size_t>(percentile * static_cast<double>(ticks.size())));
    return static_cast<double>(ticks[index]) * g_ns_per_tick / 1000.0;
  };
  const long double total =
      std::accumulate(ticks.begin(), ticks.end(), static_cast<long double>(0));
  return Distribution{static_cast<double>(ticks.front()) * g_ns_per_tick / 1000.0,
                      at(0.50), at(0.90), at(0.99), at(0.999),
                      static_cast<double>(total / ticks.size()) * g_ns_per_tick /
                          1000.0};
}

struct Measurement {
  Distribution acknowledgement;
  std::optional<Distribution> delivery;
  double operations_per_second{0};
  std::size_t samples{0};
};

void atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
}

template <class Publisher, class Subscriber>
[[nodiscard]] Measurement measure_delivery(
    Publisher& publisher, const std::vector<std::unique_ptr<Subscriber>>& subscribers,
    std::string_view channel, std::string_view payload, std::size_t warmup,
    std::size_t samples, long long expected_deliveries, int first_listener_cpu) {
  if (subscribers.empty()) {
    throw std::runtime_error("delivery measurement requires a subscriber");
  }
  const std::size_t iterations = warmup + samples;
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<std::size_t> arrived{0};
  std::atomic<std::uint64_t> last_tick{0};
  std::vector<std::thread> readers;
  readers.reserve(subscribers.size());
  for (std::size_t index = 0; index < subscribers.size(); ++index) {
    readers.emplace_back([&, index] {
      pin_to_cpu(listener_cpu(first_listener_cpu, index));
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto message = subscribers[index]->read_pubsub();
        if (message.channel != channel || message.payload != payload) {
          std::fprintf(stderr,
                       "fatal: subscriber received the wrong channel or payload\n");
          std::abort();
        }
        atomic_max(last_tick, hardware_ticks());
        arrived.fetch_add(1, std::memory_order_release);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != subscribers.size()) {
    std::this_thread::yield();
  }
  go.store(true, std::memory_order_release);

  std::vector<std::uint64_t> ack_ticks;
  std::vector<std::uint64_t> delivery_ticks;
  ack_ticks.reserve(samples);
  delivery_ticks.reserve(samples);
  auto measured_start = Clock::now();
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    arrived.store(0, std::memory_order_relaxed);
    last_tick.store(0, std::memory_order_relaxed);
    if (iteration == warmup) {
      measured_start = Clock::now();
    }
    const auto start = hardware_ticks();
    const auto delivered = publisher.publish(channel, payload);
    const auto acknowledged = hardware_ticks();
    if (delivered != expected_deliveries) {
      throw std::runtime_error("PUBLISH returned " + std::to_string(delivered) +
                               ", expected " +
                               std::to_string(expected_deliveries));
    }
    while (arrived.load(std::memory_order_acquire) != subscribers.size()) {
#if defined(__x86_64__)
      _mm_pause();
#else
      std::this_thread::yield();
#endif
    }
    const auto completed = last_tick.load(std::memory_order_relaxed);
    if (iteration >= warmup) {
      ack_ticks.push_back(acknowledged - start);
      delivery_ticks.push_back(completed - start);
    }
  }
  const auto measured_end = Clock::now();
  for (auto& reader : readers) {
    reader.join();
  }
  const double seconds = std::chrono::duration<double>(measured_end - measured_start).count();
  return Measurement{summarize_ticks(std::move(ack_ticks)),
                     summarize_ticks(std::move(delivery_ticks)),
                     static_cast<double>(samples) / seconds, samples};
}

template <class Publisher>
[[nodiscard]] Measurement measure_no_delivery(Publisher& publisher,
                                              std::string_view channel,
                                              std::string_view payload,
                                              std::size_t warmup,
                                              std::size_t samples) {
  std::vector<std::uint64_t> ack_ticks;
  ack_ticks.reserve(samples);
  auto measured_start = Clock::now();
  for (std::size_t iteration = 0; iteration < warmup + samples; ++iteration) {
    if (iteration == warmup) {
      measured_start = Clock::now();
    }
    const auto start = hardware_ticks();
    const auto delivered = publisher.publish(channel, payload);
    const auto acknowledged = hardware_ticks();
    if (delivered != 0) {
      throw std::runtime_error("no-delivery PUBLISH unexpectedly reached " +
                               std::to_string(delivered) + " subscriptions");
    }
    if (iteration >= warmup) {
      ack_ticks.push_back(acknowledged - start);
    }
  }
  const auto measured_end = Clock::now();
  const double seconds = std::chrono::duration<double>(measured_end - measured_start).count();
  return Measurement{summarize_ticks(std::move(ack_ticks)), std::nullopt,
                     static_cast<double>(samples) / seconds, samples};
}

struct Result {
  std::string engine;
  std::string transport;
  std::string scenario;
  std::string detail;
  std::size_t clients{0};
  std::size_t literal_subscriptions{0};
  std::size_t pattern_subscriptions{0};
  std::size_t payload_bytes{0};
  long long deliveries_per_publish{0};
  Measurement measurement;
  std::uint64_t baseline_rss{0};
  std::uint64_t subscribed_rss{0};
};

void print_csv_header() {
  std::cout
      << "engine,transport,scenario,detail,clients,literal_subscriptions,"
         "pattern_subscriptions,payload_bytes,deliveries_per_publish,samples,"
         "operations_per_second,ack_min_us,ack_p50_us,ack_p90_us,ack_p99_us,"
         "ack_p999_us,ack_mean_us,delivery_min_us,delivery_p50_us,"
         "delivery_p90_us,delivery_p99_us,delivery_p999_us,delivery_mean_us,"
         "baseline_rss_mib,subscribed_rss_mib,rss_delta_mib\n";
}

void print_distribution(const std::optional<Distribution>& distribution) {
  if (!distribution) {
    std::cout << ",,,,,,";
    return;
  }
  const auto& value = *distribution;
  std::cout << ',' << value.minimum_us << ',' << value.p50_us << ',' << value.p90_us
            << ',' << value.p99_us << ',' << value.p999_us << ',' << value.mean_us;
}

void emit_result(const Result& result) {
  const auto& ack = result.measurement.acknowledgement;
  constexpr double mib = 1024.0 * 1024.0;
  std::cout << result.engine << ',' << result.transport << ',' << result.scenario << ','
            << result.detail << ',' << result.clients << ','
            << result.literal_subscriptions << ',' << result.pattern_subscriptions << ','
            << result.payload_bytes << ',' << result.deliveries_per_publish << ','
            << result.measurement.samples << ','
            << result.measurement.operations_per_second << ',' << ack.minimum_us << ','
            << ack.p50_us << ',' << ack.p90_us << ',' << ack.p99_us << ','
            << ack.p999_us << ',' << ack.mean_us;
  print_distribution(result.measurement.delivery);
  std::cout << ',' << static_cast<double>(result.baseline_rss) / mib << ','
            << static_cast<double>(result.subscribed_rss) / mib << ','
            << (static_cast<double>(result.subscribed_rss) -
                static_cast<double>(result.baseline_rss)) /
                   mib
            << '\n';
  std::cout.flush();
  std::cerr << "  " << std::left << std::setw(18) << result.scenario << ' '
            << std::setw(18) << result.detail << " ack p50=" << std::right
            << std::fixed << std::setprecision(3) << ack.p50_us << " us";
  if (result.measurement.delivery) {
    std::cerr << " delivery p50=" << result.measurement.delivery->p50_us << " us";
  }
  std::cerr << " rate=" << std::setprecision(0)
            << result.measurement.operations_per_second << "/s\n";
}

template <class Client, class Maker>
[[nodiscard]] std::vector<std::unique_ptr<Client>> make_clients(
    std::size_t count, Maker&& maker) {
  std::vector<std::unique_ptr<Client>> clients;
  clients.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    clients.push_back(std::make_unique<Client>(maker(i)));
  }
  return clients;
}

// std::unique_ptr vectors are not copyable; this lightweight view lets the same
// measurement routine address a subset without transferring ownership.
template <class Client>
class ClientRef {
 public:
  explicit ClientRef(Client& client) : client_(&client) {}
  [[nodiscard]] Delivery read_pubsub() { return client_->read_pubsub(); }

 private:
  Client* client_;
};

template <class Client>
[[nodiscard]] std::vector<std::unique_ptr<ClientRef<Client>>> client_refs(
    std::vector<std::unique_ptr<Client>>& clients, std::size_t count) {
  if (count > clients.size()) {
    throw std::runtime_error("not enough clients for requested fanout");
  }
  std::vector<std::unique_ptr<ClientRef<Client>>> refs;
  refs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    refs.push_back(std::make_unique<ClientRef<Client>>(*clients[i]));
  }
  return refs;
}

[[nodiscard]] std::string transport_name(const EngineSpec& engine) {
  return engine.uses_ring() ? "SBE/ring" : "RESP2/UDS";
}

template <class Client>
void subscribe_batched(Client& client, const std::vector<std::string>& names,
                       bool patterns = false) {
  // A setup request must share the same 4 KiB ring as measured traffic. Small
  // batches preserve the workload while keeping each SBE frame bounded.
  constexpr std::size_t kBatchSize = 32;
  for (std::size_t begin = 0; begin < names.size(); begin += kBatchSize) {
    const auto end = std::min(begin + kBatchSize, names.size());
    std::vector<std::string> batch(names.begin() + begin, names.begin() + end);
    if (patterns) {
      client.psubscribe(batch);
    } else {
      client.subscribe(batch);
    }
  }
}

template <class Client>
void latency_group(const EngineSpec& engine, const RunConfig& config,
                   ServerProcess& server,
                   std::vector<std::unique_ptr<Client>>& clients) {
  auto& subscriber = clients.at(0);
  auto& publisher = clients.at(1);
  subscriber->subscribe({std::string(kLiteralChannel)});
  const auto subscribed_rss = server.resident_bytes();
  const std::vector<std::size_t> payload_sizes =
      config.smoke ? std::vector<std::size_t>{16, 256}
                   : std::vector<std::size_t>{16, 256, 4096};
  for (const auto payload_bytes : payload_sizes) {
    if (engine.uses_ring() && payload_bytes == 4096) {
      std::cerr << "  -- one-to-one 4096B: N/A (payload plus SBE framing "
                   "exceeds a 4 KiB ring)\n";
      continue;
    }
    const std::string payload(payload_bytes, 'x');
    auto refs = client_refs(clients, 1);
    const auto measurement = measure_delivery(
        *publisher, refs, kLiteralChannel, payload, config.latency_warmup,
        config.latency_samples, 1, config.first_listener_cpu);
    emit_result(Result{engine.label,
                       transport_name(engine),
                       "one-to-one",
                       std::to_string(payload_bytes) + "B",
                       1,
                       1,
                       0,
                       payload_bytes,
                       1,
                       measurement,
                       server.baseline_rss(),
                       subscribed_rss});
  }
}

template <class Client>
void fanout_case(const EngineSpec& engine, const RunConfig& config,
                 ServerProcess& server,
                 std::vector<std::unique_ptr<Client>>& clients,
                 std::size_t subscriber_count) {
  auto& publisher = clients.at(subscriber_count);
  for (std::size_t i = 0; i < subscriber_count; ++i) {
    clients[i]->subscribe({std::string(kLiteralChannel)});
  }
  const auto subscribed_rss = server.resident_bytes();
  const std::string payload(32, 'f');
  auto refs = client_refs(clients, subscriber_count);
  const auto measurement = measure_delivery(
      *publisher, refs, kLiteralChannel, payload, config.fanout_warmup,
      config.fanout_samples, static_cast<long long>(subscriber_count),
      config.first_listener_cpu);
  emit_result(Result{engine.label,
                     transport_name(engine),
                     "literal-fanout",
                     std::to_string(subscriber_count) + "-listeners",
                     subscriber_count,
                     subscriber_count,
                     0,
                     payload.size(),
                     static_cast<long long>(subscriber_count),
                     measurement,
                     server.baseline_rss(),
                     subscribed_rss});
}

[[nodiscard]] std::string numbered_name(std::string_view prefix, std::size_t value,
                                        std::string_view suffix = {}) {
  std::ostringstream out;
  out << prefix << std::setw(6) << std::setfill('0') << value << suffix;
  return out.str();
}

template <class Client>
void exact_routing_group(const EngineSpec& engine, const RunConfig& config,
                         ServerProcess& server,
                         std::vector<std::unique_ptr<Client>>& clients) {
  auto& subscriber = clients.at(0);
  auto& publisher = clients.at(1);
  const std::vector<std::size_t> counts =
      config.smoke ? std::vector<std::size_t>{1, 32}
                   : std::vector<std::size_t>{1, 1024, 16'384};
  std::size_t current = 0;
  const std::string payload(32, 'e');
  const std::string target = "bench:exact:target";
  for (const auto count : counts) {
    std::vector<std::string> additions;
    additions.reserve(count - current);
    if (current == 0) {
      additions.push_back(target);
      ++current;
    }
    while (current < count) {
      additions.push_back(numbered_name("bench:exact:", current));
      ++current;
    }
    if (!additions.empty()) {
      subscribe_batched(*subscriber, additions);
    }
    const auto subscribed_rss = server.resident_bytes();
    auto refs = client_refs(clients, 1);
    const auto measurement = measure_delivery(
        *publisher, refs, target, payload, config.routing_warmup,
        config.routing_samples, 1, config.first_listener_cpu);
    emit_result(Result{engine.label,
                       transport_name(engine),
                       "literal-routing",
                       std::to_string(count) + "-channels",
                       1,
                       count,
                       0,
                       payload.size(),
                       1,
                       measurement,
                       server.baseline_rss(),
                       subscribed_rss});
  }
}

template <class Client>
void pattern_routing_group(const EngineSpec& engine, const RunConfig& config,
                           ServerProcess& server,
                           std::vector<std::unique_ptr<Client>>& clients) {
  auto& subscriber = clients.at(0);
  auto& publisher = clients.at(1);
  const std::vector<std::size_t> counts =
      config.smoke ? std::vector<std::size_t>{1, 16}
                   : std::vector<std::size_t>{1, 64, 1024, 8192};
  const std::string payload(32, 'p');

  const auto empty_measurement = measure_no_delivery(
      *publisher, kPatternMissChannel, payload, config.routing_warmup,
      config.routing_samples);
  emit_result(Result{engine.label,
                     transport_name(engine),
                     "pattern-miss",
                     "0-patterns",
                     1,
                     0,
                     0,
                     payload.size(),
                     0,
                     empty_measurement,
                     server.baseline_rss(),
                     server.resident_bytes()});

  std::size_t current = 0;
  for (const auto count : counts) {
    std::vector<std::string> additions;
    additions.reserve(count - current);
    while (current < count) {
      additions.push_back(numbered_name("bench:pattern:", current, ":*"));
      ++current;
    }
    subscribe_batched(*subscriber, additions, true);
    const auto subscribed_rss = server.resident_bytes();
    const std::size_t samples = count >= 8192 ? config.routing_samples / 4
                                              : config.routing_samples;
    const std::size_t warmup = count >= 8192 ? config.routing_warmup / 4
                                             : config.routing_warmup;
    const auto miss = measure_no_delivery(*publisher, kPatternMissChannel, payload,
                                          std::max<std::size_t>(warmup, 1),
                                          std::max<std::size_t>(samples, 1));
    emit_result(Result{engine.label,
                       transport_name(engine),
                       "pattern-miss",
                       std::to_string(count) + "-patterns",
                       1,
                       0,
                       count,
                       payload.size(),
                       0,
                       miss,
                       server.baseline_rss(),
                       subscribed_rss});

    const auto target = numbered_name("bench:pattern:", count - 1, ":hit");
    auto refs = client_refs(clients, 1);
    const auto hit = measure_delivery(
        *publisher, refs, target, payload, std::max<std::size_t>(warmup, 1),
        std::max<std::size_t>(samples, 1), 1, config.first_listener_cpu);
    emit_result(Result{engine.label,
                       transport_name(engine),
                       "pattern-hit",
                       std::to_string(count) + "-patterns",
                       1,
                       0,
                       count,
                       payload.size(),
                       1,
                       hit,
                       server.baseline_rss(),
                       subscribed_rss});
  }
}

template <class Client>
void many_literal_group(const EngineSpec& engine, const RunConfig& config,
                        ServerProcess& server,
                        std::vector<std::unique_ptr<Client>>& clients,
                        std::size_t subscriber_count,
                        std::size_t private_per_client) {
  auto& publisher = clients.at(subscriber_count);
  const std::string shared_literal = "bench:mixed:literal:shared";
  for (std::size_t client = 0; client < subscriber_count; ++client) {
    std::vector<std::string> channels{shared_literal};
    channels.reserve(private_per_client + 1);
    for (std::size_t item = 0; item < private_per_client; ++item) {
      channels.push_back("bench:mixed:literal:" + std::to_string(client) + ":" +
                         std::to_string(item));
    }
    subscribe_batched(*clients[client], channels);
  }
  const auto subscribed_rss = server.resident_bytes();
  const std::string payload(32, 'm');
  const auto edges = subscriber_count * (private_per_client + 1);

  auto literal_refs = client_refs(clients, subscriber_count);
  const auto literal = measure_delivery(
      *publisher, literal_refs, shared_literal, payload, config.fanout_warmup,
      config.fanout_samples, static_cast<long long>(subscriber_count),
      config.first_listener_cpu);
  emit_result(Result{engine.label,
                     transport_name(engine),
                     "many-literal",
                     std::to_string(subscriber_count) + "x" +
                         std::to_string(private_per_client),
                     subscriber_count,
                     edges,
                     0,
                     payload.size(),
                     static_cast<long long>(subscriber_count),
                     literal,
                     server.baseline_rss(),
                     subscribed_rss});
}

template <class Client>
void many_pattern_group(const EngineSpec& engine, const RunConfig& config,
                        ServerProcess& server,
                        std::vector<std::unique_ptr<Client>>& clients,
                        std::size_t subscriber_count,
                        std::size_t private_per_client) {
  auto& publisher = clients.at(subscriber_count);
  const std::string shared_pattern = "bench:mixed:pattern:shared:*";
  for (std::size_t client = 0; client < subscriber_count; ++client) {
    std::vector<std::string> patterns{shared_pattern};
    patterns.reserve(private_per_client + 1);
    for (std::size_t item = 0; item < private_per_client; ++item) {
      patterns.push_back("bench:mixed:pattern:" + std::to_string(client) + ":" +
                         std::to_string(item) + ":*");
    }
    subscribe_batched(*clients[client], patterns, true);
  }
  const auto subscribed_rss = server.resident_bytes();
  const std::string payload(32, 'm');
  const auto edges = subscriber_count * (private_per_client + 1);
  auto pattern_refs = client_refs(clients, subscriber_count);
  const auto pattern = measure_delivery(
      *publisher, pattern_refs, "bench:mixed:pattern:shared:hit", payload,
      config.fanout_warmup, config.fanout_samples,
      static_cast<long long>(subscriber_count), config.first_listener_cpu);
  emit_result(Result{engine.label,
                     transport_name(engine),
                     "many-pattern",
                     std::to_string(subscriber_count) + "x" +
                         std::to_string(private_per_client),
                     subscriber_count,
                     0,
                     edges,
                     payload.size(),
                     static_cast<long long>(subscriber_count),
                     pattern,
                     server.baseline_rss(),
                     subscribed_rss});
}

[[nodiscard]] bool unsupported_pattern_command(const std::exception& error) {
  const std::string_view message = error.what();
  return message.find("PSUBSCRIBE") != std::string_view::npos &&
         message.find("unknown command") != std::string_view::npos;
}

template <class Client, class Maker, class Work>
void with_clients(std::size_t count, Maker&& maker, Work&& work) {
  auto clients = make_clients<Client>(count, std::forward<Maker>(maker));
  work(clients);
}

template <class Work>
void with_scenario(const EngineSpec& engine, const RunConfig& config,
                   std::size_t client_count, std::string_view case_name,
                   Work&& work) {
  const auto ring_count = engine.uses_ring() ? client_count : 0;
  ServerProcess server(engine, config, ring_count, case_name);
  if (engine.uses_ring()) {
    with_clients<RingConnection>(
        client_count,
        [&](std::size_t index) {
          return RingConnection::open(server.ring_paths().at(index));
        },
        [&](auto& clients) { work(server, clients); });
  } else {
    with_clients<RespConnection>(
        client_count,
        [&](std::size_t) {
          return RespConnection::connect_uds(server.socket_path(),
                                             std::chrono::seconds(10));
        },
        [&](auto& clients) { work(server, clients); });
  }
}

void run_engine(const EngineSpec& engine, const RunConfig& config) {
  std::cerr << "\n[" << engine.label << "] " << kind_name(engine.kind) << " / "
            << engine.binary << '\n';
  with_scenario(engine, config, 2, "latency", [&](auto& server, auto& clients) {
    latency_group(engine, config, server, clients);
  });

  const std::vector<std::size_t> fanouts =
      config.smoke ? std::vector<std::size_t>{1, 4}
                   : std::vector<std::size_t>{1, 8, 32, 64};
  for (const auto fanout : fanouts) {
    with_scenario(engine, config, fanout + 1, "fanout", [&](auto& server, auto& clients) {
      fanout_case(engine, config, server, clients, fanout);
    });
  }

  with_scenario(engine, config, 2, "exact-routing", [&](auto& server, auto& clients) {
    exact_routing_group(engine, config, server, clients);
  });
  try {
    with_scenario(engine, config, 2, "pattern-routing",
                  [&](auto& server, auto& clients) {
                    pattern_routing_group(engine, config, server, clients);
                  });
  } catch (const std::exception& error) {
    if (!unsupported_pattern_command(error)) {
      throw;
    }
    std::cerr << "  -- pattern-routing: N/A (PSUBSCRIBE unsupported)\n";
  }

  const std::size_t mixed_clients = config.smoke ? 4 : 32;
  const std::size_t mixed_private = config.smoke ? 4 : 32;
  with_scenario(engine, config, mixed_clients + 1, "many-literal",
                [&](auto& server, auto& clients) {
                  many_literal_group(engine, config, server, clients, mixed_clients,
                                     mixed_private);
                });
  try {
    with_scenario(engine, config, mixed_clients + 1, "many-pattern",
                  [&](auto& server, auto& clients) {
                    many_pattern_group(engine, config, server, clients, mixed_clients,
                                       mixed_private);
                  });
  } catch (const std::exception& error) {
    if (!unsupported_pattern_command(error)) {
      throw;
    }
    std::cerr << "  -- many-pattern: N/A (PSUBSCRIBE unsupported)\n";
  }
}

void print_usage(std::string_view program) {
  std::cerr
      << "usage: " << program
      << " --engine LABEL:KIND:PATH [--engine ...] [options]\n"
         "\n"
         "KIND: goblin-ring | goblin-uds | redis | dragonfly | mini\n"
         "options:\n"
         "  --parity-config PATH\n"
         "  --server-cpu N --publisher-cpu N --first-listener-cpu N\n"
         "  --latency-samples N --fanout-samples N --routing-samples N\n"
         "  --smoke   smaller scales and sample counts for correctness checks\n";
}

[[nodiscard]] RunConfig parse_arguments(int argc, char** argv) {
  RunConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&](std::string_view option) -> std::string_view {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string(option) + " requires a value");
      }
      return argv[++i];
    };
    if (arg == "--engine") {
      config.engines.push_back(parse_engine(next(arg)));
    } else if (arg == "--parity-config") {
      config.parity_config = next(arg);
    } else if (arg == "--server-cpu") {
      config.server_cpu = parse_cpu(next(arg), arg);
    } else if (arg == "--publisher-cpu") {
      config.publisher_cpu = parse_cpu(next(arg), arg);
    } else if (arg == "--first-listener-cpu") {
      config.first_listener_cpu = parse_cpu(next(arg), arg);
    } else if (arg == "--latency-samples") {
      config.latency_samples = parse_size(next(arg), arg);
    } else if (arg == "--fanout-samples") {
      config.fanout_samples = parse_size(next(arg), arg);
    } else if (arg == "--routing-samples") {
      config.routing_samples = parse_size(next(arg), arg);
    } else if (arg == "--smoke") {
      config.smoke = true;
      config.latency_samples = 500;
      config.latency_warmup = 100;
      config.fanout_samples = 200;
      config.fanout_warmup = 50;
      config.routing_samples = 200;
      config.routing_warmup = 50;
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + std::string(arg));
    }
  }
  if (config.engines.empty()) {
    throw std::runtime_error("at least one --engine is required");
  }
  if (!fs::exists(config.parity_config)) {
    throw std::runtime_error("parity config does not exist: " +
                             config.parity_config.string());
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::signal(SIGPIPE, SIG_IGN);
    auto config = parse_arguments(argc, argv);
    calibrate_ticks();
    pin_to_cpu(config.publisher_cpu);
    std::cerr << "Pub/Sub benchmark: " << std::fixed << std::setprecision(6)
              << g_ns_per_tick << " ns/tick, server CPU " << config.server_cpu
              << ", publisher CPU " << config.publisher_cpu << ", listener CPUs from "
              << config.first_listener_cpu << '\n';
    print_csv_header();
    bool failed = false;
    for (const auto& engine : config.engines) {
      try {
        run_engine(engine, config);
      } catch (const std::exception& error) {
        failed = true;
        std::cerr << "  !! " << engine.label << " failed: " << error.what() << '\n';
      }
    }
    return failed ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "pubsub_benchmark: " << error.what() << '\n';
    print_usage(argv[0]);
    return 2;
  }
}
