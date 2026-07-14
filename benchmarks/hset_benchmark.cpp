// Native HSET speed, latency, and process-memory benchmark.
//
// Incumbents use black-box RESP/TCP. Goblin can additionally be driven through
// the typed SBE client over its shared-memory ring, using the same workloads and
// correctness checks. The harness writes JSON plus Markdown results.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kRandomTokenDigits = 12;

struct EngineSpec {
  std::string label;
  std::string kind;
  fs::path binary;

  [[nodiscard]] bool is_goblin() const {
    return kind == "goblin" || kind == "goblin-sbe";
  }
  [[nodiscard]] bool is_sbe() const { return kind == "goblin-sbe"; }
};

struct Config {
  std::vector<EngineSpec> engines;
  fs::path redis_benchmark;
  fs::path parity_config = fs::path(__FILE__).parent_path() / "redis-parity.conf";
  std::vector<std::string> goblin_args;
  std::int64_t large_fields = 1'000'000;
  std::int64_t small_total_fields = 500'000;
  std::vector<int> small_shapes{8, 32, 64, 128};
  int value_bytes = 16;
  int grown_value_bytes = 64;
  int hset_batch = 128;
  int pipeline = 256;
  int requests = 500'000;
  int rounds = 3;
  int warmup_requests = 10'000;
  int construction_rounds = 3;
  int baseline_warmup_commands = 16;
  int baseline_warmup_fields = 2'048;
  double optimize_density = 0.97;
  std::int64_t relocation_fields = 100'000;
  std::vector<double> relocation_densities{0.0, 0.001, 0.01, 0.1, 0.5, 1.0};
  int relocation_rounds = 1;
  int mixed_fields = 100'000;
  int mixed_samples = 10'000;
  int mixed_warmup = 1'000;
  int mixed_rounds = 1;
  std::array<int, 4> mixed_weights{25, 25, 25, 25};
  std::uint64_t seed = 0xC0FFEE;
  double settle_seconds = 0.5;
  double timeout_seconds = 600.0;
  int server_core = -1;
  int client_core = -1;
  bool skip_large = false;
  bool skip_small = false;
  bool skip_relocation = false;
  bool skip_mixed = false;
  fs::path output_json = "benchmark-results/hset.json";
  fs::path report = "benchmark-results/hset.md";
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

std::string errno_message(std::string_view action) {
  return std::string(action) + ": " + std::strerror(errno);
}

std::string next_arg(int argc, char** argv, int& index) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + argv[index]);
  return argv[++index];
}

std::vector<std::string> split(std::string_view text, char delimiter) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find(delimiter, start);
    result.emplace_back(text.substr(start, end == std::string_view::npos
                                              ? text.size() - start
                                              : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

EngineSpec parse_engine(std::string_view text) {
  const auto first = text.find(':');
  const auto second = first == std::string_view::npos
                          ? std::string_view::npos
                          : text.find(':', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos) {
    fail("--engine expects LABEL:KIND:PATH");
  }
  EngineSpec spec{std::string(text.substr(0, first)),
                  std::string(text.substr(first + 1, second - first - 1)),
                  std::string(text.substr(second + 1))};
  if (spec.kind != "goblin" && spec.kind != "goblin-sbe" && spec.kind != "redis" &&
      spec.kind != "dragonfly" && spec.kind != "mini-redis-go") {
    fail("engine kind must be goblin, goblin-sbe, redis, dragonfly, or mini-redis-go");
  }
  return spec;
}

std::vector<int> parse_int_list(std::string_view text) {
  std::vector<int> result;
  for (const auto& part : split(text, ',')) result.push_back(std::stoi(part));
  return result;
}

std::vector<double> parse_double_list(std::string_view text) {
  std::vector<double> result;
  for (const auto& part : split(text, ',')) result.push_back(std::stod(part));
  return result;
}

void print_help(const char* program) {
  std::cout
      << "Usage: " << program << " --engine LABEL:KIND:PATH ... "
      << "--redis-benchmark PATH [options]\n"
      << "  --large-fields N              default 1000000\n"
      << "  --small-total-fields N        default 500000\n"
      << "  --small-shapes CSV            default 8,32,64,128\n"
      << "  --requests N                  point-probe requests\n"
      << "  --rounds N                    point-probe repetitions\n"
      << "  --construction-rounds N       fresh-server load/RSS rounds\n"
      << "  --relocation-fields N\n"
      << "  --relocation-rounds N\n"
      << "  --mixed-samples N\n"
      << "  --mixed-rounds N\n"
      << "  --goblin-arg VALUE            repeatable\n"
      << "  --server-core N               pin each server on Linux\n"
      << "  --client-core N               pin this harness and child clients on Linux\n"
      << "  --output-json PATH\n"
      << "  --report PATH\n";
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_help(argv[0]);
      std::exit(0);
    } else if (arg == "--engine") {
      config.engines.push_back(parse_engine(next_arg(argc, argv, i)));
    } else if (arg == "--redis-benchmark") {
      config.redis_benchmark = next_arg(argc, argv, i);
    } else if (arg == "--parity-config") {
      config.parity_config = next_arg(argc, argv, i);
    } else if (arg == "--goblin-arg") {
      config.goblin_args.push_back(next_arg(argc, argv, i));
    } else if (arg.rfind("--goblin-arg=", 0) == 0) {
      config.goblin_args.push_back(arg.substr(std::strlen("--goblin-arg=")));
    } else if (arg == "--large-fields") {
      config.large_fields = std::stoll(next_arg(argc, argv, i));
    } else if (arg == "--small-total-fields") {
      config.small_total_fields = std::stoll(next_arg(argc, argv, i));
    } else if (arg == "--small-shapes") {
      config.small_shapes = parse_int_list(next_arg(argc, argv, i));
    } else if (arg == "--value-bytes") {
      config.value_bytes = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--grown-value-bytes") {
      config.grown_value_bytes = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--hset-batch") {
      config.hset_batch = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--pipeline") {
      config.pipeline = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--requests") {
      config.requests = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--rounds") {
      config.rounds = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--warmup-requests") {
      config.warmup_requests = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--construction-rounds") {
      config.construction_rounds = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--baseline-warmup-commands") {
      config.baseline_warmup_commands = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--baseline-warmup-fields") {
      config.baseline_warmup_fields = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--optimize-density") {
      config.optimize_density = std::stod(next_arg(argc, argv, i));
    } else if (arg == "--relocation-fields") {
      config.relocation_fields = std::stoll(next_arg(argc, argv, i));
    } else if (arg == "--relocation-densities") {
      config.relocation_densities = parse_double_list(next_arg(argc, argv, i));
    } else if (arg == "--relocation-rounds") {
      config.relocation_rounds = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-fields") {
      config.mixed_fields = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-samples") {
      config.mixed_samples = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-warmup") {
      config.mixed_warmup = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-rounds") {
      config.mixed_rounds = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-insert-weight") {
      config.mixed_weights[0] = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-update-weight") {
      config.mixed_weights[1] = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-delete-weight") {
      config.mixed_weights[2] = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--mixed-grow-weight") {
      config.mixed_weights[3] = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--seed") {
      config.seed = std::stoull(next_arg(argc, argv, i), nullptr, 0);
    } else if (arg == "--settle-seconds") {
      config.settle_seconds = std::stod(next_arg(argc, argv, i));
    } else if (arg == "--timeout") {
      config.timeout_seconds = std::stod(next_arg(argc, argv, i));
    } else if (arg == "--server-core") {
      config.server_core = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--client-core") {
      config.client_core = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--skip-large") {
      config.skip_large = true;
    } else if (arg == "--skip-small") {
      config.skip_small = true;
    } else if (arg == "--skip-relocation") {
      config.skip_relocation = true;
    } else if (arg == "--skip-mixed") {
      config.skip_mixed = true;
    } else if (arg == "--output-json") {
      config.output_json = next_arg(argc, argv, i);
    } else if (arg == "--report") {
      config.report = next_arg(argc, argv, i);
    } else {
      fail("unknown argument: " + arg);
    }
  }
  if (config.engines.empty()) fail("at least one --engine is required");
  if (config.redis_benchmark.empty()) fail("--redis-benchmark is required");
  if (!fs::exists(config.redis_benchmark)) fail("redis-benchmark not found");
  for (const auto& engine : config.engines) {
    if (!fs::exists(engine.binary)) fail("engine binary not found: " + engine.binary.string());
  }
  if (config.skip_large && config.skip_small && config.skip_relocation &&
      config.skip_mixed) {
    fail("at least one workload must be enabled");
  }
  if (config.value_bytes <= 1 || config.grown_value_bytes <= config.value_bytes ||
      config.grown_value_bytes > 65'534) {
    fail("invalid initial/grown value widths");
  }
  if (config.pipeline <= 0 || config.hset_batch <= 0 || config.requests <= 0 ||
      config.rounds <= 0 || config.construction_rounds <= 0 ||
      config.relocation_rounds <= 0 || config.mixed_rounds <= 0) {
    fail("counts, pipeline depths, and rounds must be positive");
  }
  if (config.large_fields <= 0 || config.small_total_fields <= 0 ||
      config.relocation_fields <= 0 || config.mixed_fields <= 0 ||
      config.mixed_samples <= 0 || config.mixed_warmup < 0 ||
      config.small_shapes.empty() ||
      std::ranges::any_of(config.small_shapes, [](int shape) { return shape <= 0; })) {
    fail("workload sizes and hash shapes must be positive");
  }
  if (config.relocation_densities.empty() ||
      std::ranges::any_of(config.relocation_densities,
                          [](double density) { return density < 0.0 || density > 1.0; })) {
    fail("relocation densities must be in [0, 1]");
  }
  if (std::ranges::any_of(config.mixed_weights,
                          [](int weight) { return weight < 0; }) ||
      std::accumulate(config.mixed_weights.begin(), config.mixed_weights.end(), 0) == 0) {
    fail("mixed workload weights must be non-negative with a positive sum");
  }
  if (config.settle_seconds < 0.0 || config.timeout_seconds <= 0.0) {
    fail("settle time must be non-negative and timeout must be positive");
  }
  if (config.server_core < -1 || config.client_core < -1) {
    fail("CPU cores must be non-negative or -1 for unpinned");
  }
  return config;
}

void pin_current_process(int core) {
  if (core < 0) return;
#ifdef __linux__
  if (core >= CPU_SETSIZE) fail("CPU core is outside cpu_set_t range");
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    fail(errno_message("sched_setaffinity"));
  }
#else
  (void)core;
#endif
}

struct RespValue {
  enum class Type { null_value, integer, string, array };
  Type type = Type::null_value;
  std::int64_t integer = 0;
  std::string string;
  std::vector<RespValue> array;

  [[nodiscard]] bool is_integer(std::int64_t expected) const {
    return type == Type::integer && integer == expected;
  }
};

void append_number(std::string& output, std::int64_t value) {
  std::array<char, 32> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) fail("integer formatting failed");
  output.append(buffer.data(), result.ptr);
}

void append_command(std::string& output, const std::vector<std::string>& parts) {
  output.push_back('*');
  append_number(output, static_cast<std::int64_t>(parts.size()));
  output += "\r\n";
  for (const auto& part : parts) {
    output.push_back('$');
    append_number(output, static_cast<std::int64_t>(part.size()));
    output += "\r\n";
    output += part;
    output += "\r\n";
  }
}

class RespClient {
 public:
  RespClient(int port, double timeout_seconds) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) fail(errno_message("socket"));
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const auto micros = static_cast<long>(timeout_seconds * 1'000'000.0);
    timeval timeout{static_cast<time_t>(micros / 1'000'000),
                    static_cast<suseconds_t>(micros % 1'000'000)};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      const auto message = errno_message("connect");
      ::close(fd_);
      fd_ = -1;
      fail(message);
    }
  }

  RespClient(const fs::path& ring_path, double timeout_seconds) {
    const auto wait = std::chrono::milliseconds(
        static_cast<long long>(std::ceil(timeout_seconds * 1000.0)));
    sbe_ = goblin::core::SbeRingClient::open(ring_path.c_str(), wait);
    if (!sbe_) fail("could not open SBE ring: " + ring_path.string());
  }

  RespClient(const RespClient&) = delete;
  RespClient& operator=(const RespClient&) = delete;
  ~RespClient() {
    if (fd_ >= 0) ::close(fd_);
  }

  RespValue command(const std::vector<std::string>& parts) {
    if (sbe_) return sbe_command(parts);
    std::string wire;
    wire.reserve(128);
    append_command(wire, parts);
    send_all(wire);
    return read_response();
  }

  std::pair<std::int64_t, double> pipeline(
      std::int64_t count, int depth,
      const std::function<std::vector<std::string>(std::int64_t)>& build) {
    if (sbe_) {
      const auto started = Clock::now();
      for (std::int64_t index = 0; index < count; ++index) {
        (void)command(build(index));
      }
      return {count,
              std::chrono::duration<double>(Clock::now() - started).count()};
    }
    const auto started = Clock::now();
    std::string wire;
    wire.reserve(static_cast<std::size_t>(depth) * 256);
    int pending = 0;
    for (std::int64_t index = 0; index < count; ++index) {
      append_command(wire, build(index));
      ++pending;
      if (pending == depth) {
        send_all(wire);
        wire.clear();
        for (int reply = 0; reply < pending; ++reply) (void)read_response();
        pending = 0;
      }
    }
    if (pending != 0) {
      send_all(wire);
      for (int reply = 0; reply < pending; ++reply) (void)read_response();
    }
    return {count, std::chrono::duration<double>(Clock::now() - started).count()};
  }

  double benchmark_sbe_point(const std::vector<std::string>& command,
                             int requests, std::int64_t keyspace,
                             std::uint64_t seed) {
    if (!sbe_) fail("SBE point benchmark requested for a RESP client");
    if (command.empty()) fail("empty SBE point command");
    std::uint64_t state = seed;
    std::array<char, 96> key_buffer{};
    std::array<char, 96> field_buffer{};
    const auto started = Clock::now();
    for (int request = 0; request < requests; ++request) {
      state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
      const auto random = static_cast<std::int64_t>(
          state % static_cast<std::uint64_t>(std::max<std::int64_t>(1, keyspace)));
      const auto key = render_random_token(command.at(1), random, key_buffer);
      if (command[0] == "HSET") {
        const auto field = render_random_token(command.at(2), random, field_buffer);
        const std::array<std::pair<std::string_view, std::string_view>, 1> entries{{
            {field, command.at(3)}}};
        (void)sbe_->hset(key, entries);
      } else if (command[0] == "HGET") {
        const auto field = render_random_token(command.at(2), random, field_buffer);
        (void)sbe_->hget(key, field);
      } else {
        fail("unsupported SBE point command: " + command[0]);
      }
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    return static_cast<double>(requests) / seconds;
  }

 private:
  int fd_ = -1;
  std::optional<goblin::core::SbeRingClient> sbe_;
  std::string input_;
  std::size_t input_pos_ = 0;

  static RespValue integer(std::int64_t value) {
    RespValue response;
    response.type = RespValue::Type::integer;
    response.integer = value;
    return response;
  }

  static RespValue string(std::string value) {
    RespValue response;
    response.type = RespValue::Type::string;
    response.string = std::move(value);
    return response;
  }

  static std::string_view render_random_token(
      std::string_view pattern, std::int64_t value, std::array<char, 96>& output) {
    constexpr std::string_view token = "__rand_int__";
    const auto position = pattern.find(token);
    if (position == std::string_view::npos) return pattern;
    if (pattern.size() - token.size() + kRandomTokenDigits > output.size()) {
      fail("random-token command component is too long");
    }
    std::memcpy(output.data(), pattern.data(), position);
    std::array<char, 32> digits{};
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (converted.ec != std::errc{}) fail("random token formatting failed");
    const auto digit_count = static_cast<std::size_t>(converted.ptr - digits.data());
    if (digit_count > static_cast<std::size_t>(kRandomTokenDigits)) {
      fail("random token value exceeds its fixed width");
    }
    const auto zeroes = static_cast<std::size_t>(kRandomTokenDigits) - digit_count;
    std::memset(output.data() + position, '0', zeroes);
    std::memcpy(output.data() + position + zeroes, digits.data(), digit_count);
    const auto suffix = pattern.substr(position + token.size());
    std::memcpy(output.data() + position + kRandomTokenDigits,
                suffix.data(), suffix.size());
    return {output.data(), position + kRandomTokenDigits + suffix.size()};
  }

  RespValue sbe_command(const std::vector<std::string>& parts) {
    if (parts.empty()) fail("empty benchmark command");
    auto& client = *sbe_;
    const auto& verb = parts.front();
    if (verb == "PING") {
      if (!client.ping()) fail("SBE PING failed");
      return string("PONG");
    }
    if (verb == "HSET") {
      if (parts.size() < 4 || parts.size() % 2 != 0) fail("invalid HSET command");
      std::vector<std::pair<std::string_view, std::string_view>> entries;
      entries.reserve((parts.size() - 2) / 2);
      for (std::size_t index = 2; index < parts.size(); index += 2) {
        entries.emplace_back(parts[index], parts[index + 1]);
      }
      return integer(client.hset(parts[1], entries));
    }
    if (verb == "HGET") {
      const auto value = client.hget(parts.at(1), parts.at(2));
      return value ? string(*value) : RespValue{};
    }
    if (verb == "HDEL") {
      std::vector<std::string_view> fields;
      fields.reserve(parts.size() - 2);
      for (std::size_t index = 2; index < parts.size(); ++index) {
        fields.push_back(parts[index]);
      }
      return integer(client.hdel(parts.at(1), fields));
    }
    if (verb == "HLEN") return integer(client.hlen(parts.at(1)));
    if (verb == "DEL") {
      std::vector<std::string_view> keys;
      keys.reserve(parts.size() - 1);
      for (std::size_t index = 1; index < parts.size(); ++index) {
        keys.push_back(parts[index]);
      }
      return integer(client.del(keys));
    }
    if (verb == "INFO") return string(client.info());
    if (verb == "GOBLIN.OPTIMIZE") {
      const double density = parts.size() > 2 ? std::stod(parts[2]) : 0.0;
      const auto reclaimed = client.optimize(parts.at(1), density);
      return reclaimed ? integer(*reclaimed) : RespValue{};
    }
    if (verb == "GOBLIN.MEMORY") {
      const auto fields = client.memory(parts.at(1));
      if (!fields) return {};
      RespValue response;
      response.type = RespValue::Type::array;
      response.array.reserve(fields->size() * 2);
      for (const auto& [name, value] : *fields) {
        response.array.push_back(string(name));
        response.array.push_back(string(value));
      }
      return response;
    }
    fail("SBE benchmark adapter does not support command: " + verb);
  }

  void send_all(std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto result = ::send(fd_, data.data() + sent, data.size() - sent, flags);
      if (result <= 0) fail(errno_message("send"));
      sent += static_cast<std::size_t>(result);
    }
  }

  void receive_more() {
    if (input_pos_ > 0 && (input_pos_ > 65'536 || input_pos_ == input_.size())) {
      input_.erase(0, input_pos_);
      input_pos_ = 0;
    }
    std::array<char, 65'536> buffer{};
    const auto count = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (count <= 0) fail("connection closed while reading RESP response");
    input_.append(buffer.data(), static_cast<std::size_t>(count));
  }

  std::string read_exact(std::size_t size) {
    while (input_.size() - input_pos_ < size) receive_more();
    std::string result = input_.substr(input_pos_, size);
    input_pos_ += size;
    return result;
  }

  std::string read_line() {
    while (true) {
      const auto end = input_.find("\r\n", input_pos_);
      if (end != std::string::npos) {
        std::string result = input_.substr(input_pos_, end - input_pos_);
        input_pos_ = end + 2;
        return result;
      }
      receive_more();
    }
  }

  static std::int64_t parse_integer(std::string_view text) {
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      fail("invalid RESP integer");
    }
    return value;
  }

  RespValue read_response() {
    const char prefix = read_exact(1)[0];
    if (prefix == '+' || prefix == '-') {
      auto line = read_line();
      if (prefix == '-') fail("server returned error: " + line);
      RespValue value;
      value.type = RespValue::Type::string;
      value.string = std::move(line);
      return value;
    }
    if (prefix == ':') {
      RespValue value;
      value.type = RespValue::Type::integer;
      value.integer = parse_integer(read_line());
      return value;
    }
    if (prefix == '$') {
      const auto length = parse_integer(read_line());
      if (length < 0) return {};
      RespValue value;
      value.type = RespValue::Type::string;
      value.string = read_exact(static_cast<std::size_t>(length));
      if (read_exact(2) != "\r\n") fail("invalid RESP bulk terminator");
      return value;
    }
    if (prefix == '*') {
      const auto count = parse_integer(read_line());
      if (count < 0) return {};
      RespValue value;
      value.type = RespValue::Type::array;
      value.array.reserve(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) value.array.push_back(read_response());
      return value;
    }
    fail("invalid RESP prefix");
  }
};

struct CapturedProcess {
  int exit_code = -1;
  std::string output;
};

std::vector<char*> exec_argv(std::vector<std::string>& arguments) {
  std::vector<char*> result;
  result.reserve(arguments.size() + 1);
  for (auto& argument : arguments) result.push_back(argument.data());
  result.push_back(nullptr);
  return result;
}

CapturedProcess run_capture(std::vector<std::string> arguments, double timeout_seconds) {
  int pipes[2];
  if (::pipe(pipes) != 0) fail(errno_message("pipe"));
  const pid_t child = ::fork();
  if (child < 0) fail(errno_message("fork"));
  if (child == 0) {
    ::close(pipes[0]);
    ::dup2(pipes[1], STDOUT_FILENO);
    ::dup2(pipes[1], STDERR_FILENO);
    ::close(pipes[1]);
    auto pointers = exec_argv(arguments);
    ::execv(pointers[0], pointers.data());
    _exit(127);
  }
  ::close(pipes[1]);
  const int flags = ::fcntl(pipes[0], F_GETFL, 0);
  ::fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK);
  CapturedProcess result;
  const auto deadline = Clock::now() + std::chrono::duration<double>(timeout_seconds);
  bool exited = false;
  int status = 0;
  while (!exited) {
    std::array<char, 4096> buffer{};
    while (true) {
      const auto count = ::read(pipes[0], buffer.data(), buffer.size());
      if (count > 0) result.output.append(buffer.data(), static_cast<std::size_t>(count));
      else break;
    }
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) exited = true;
    else if (Clock::now() >= deadline) {
      ::kill(child, SIGKILL);
      ::waitpid(child, &status, 0);
      ::close(pipes[0]);
      fail("child process timed out");
    } else {
      pollfd descriptor{pipes[0], POLLIN, 0};
      (void)::poll(&descriptor, 1, 20);
    }
  }
  std::array<char, 4096> buffer{};
  while (true) {
    const auto count = ::read(pipes[0], buffer.data(), buffer.size());
    if (count <= 0) break;
    result.output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipes[0]);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  return result;
}

int free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) fail(errno_message("socket"));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    fail(errno_message("bind"));
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    fail(errno_message("getsockname"));
  }
  ::close(fd);
  return ntohs(address.sin_port);
}

fs::path temporary_directory(std::string_view prefix) {
  std::string pattern = "/tmp/" + std::string(prefix) + "-XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* result = ::mkdtemp(writable.data());
  if (result == nullptr) fail(errno_message("mkdtemp"));
  return result;
}

class ServerProcess {
 public:
  ServerProcess() = default;
  ServerProcess(pid_t pid, int port, fs::path temp_dir, fs::path ring_path = {})
      : pid_(pid),
        port_(port),
        temp_dir_(std::move(temp_dir)),
        ring_path_(std::move(ring_path)) {}
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;
  ServerProcess(ServerProcess&& other) noexcept { *this = std::move(other); }
  ServerProcess& operator=(ServerProcess&& other) noexcept {
    if (this != &other) {
      stop();
      pid_ = std::exchange(other.pid_, -1);
      port_ = other.port_;
      temp_dir_ = std::move(other.temp_dir_);
      ring_path_ = std::move(other.ring_path_);
    }
    return *this;
  }
  ~ServerProcess() { stop(); }

  [[nodiscard]] pid_t pid() const { return pid_; }
  [[nodiscard]] int port() const { return port_; }
  [[nodiscard]] const fs::path& ring_path() const { return ring_path_; }

  void stop() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      int status = 0;
      while (::waitpid(pid_, &status, WNOHANG) == 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (::waitpid(pid_, &status, WNOHANG) == 0) {
        ::kill(pid_, SIGKILL);
        (void)::waitpid(pid_, &status, 0);
      }
      pid_ = -1;
    }
    if (!temp_dir_.empty()) {
      std::error_code error;
      fs::remove_all(temp_dir_, error);
      temp_dir_.clear();
    }
    ring_path_.clear();
  }

 private:
  pid_t pid_ = -1;
  int port_ = 0;
  fs::path temp_dir_;
  fs::path ring_path_;
};

pid_t spawn_server(std::vector<std::string> arguments,
                   bool limit_go_runtime = false, int server_core = -1) {
  const pid_t child = ::fork();
  if (child < 0) fail(errno_message("fork"));
  if (child == 0) {
    pin_current_process(server_core);
    if (limit_go_runtime) {
      (void)::setenv("GOMAXPROCS", "1", 1);
    }
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) ::close(null_fd);
    }
    auto pointers = exec_argv(arguments);
    ::execv(pointers[0], pointers.data());
    _exit(127);
  }
  return child;
}

void wait_for_server(const ServerProcess& server) {
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (Clock::now() < deadline) {
    int status = 0;
    if (::waitpid(server.pid(), &status, WNOHANG) == server.pid()) {
      fail("server exited during startup");
    }
    try {
      RespClient client(server.port(), 1.0);
      const auto pong = client.command({"PING"});
      if (pong.type == RespValue::Type::string && pong.string == "PONG") return;
    } catch (const std::exception&) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  fail("server did not become ready");
}

ServerProcess start_engine(const EngineSpec& spec, const Config& config,
                           const std::vector<std::string>& extra_goblin_args = {}) {
  const int port = free_port();
  fs::path temp_dir;
  fs::path ring_path;
  std::vector<std::string> command{spec.binary.string()};
  if (spec.is_goblin()) {
    command.insert(command.end(), {"--port", std::to_string(port)});
    if (spec.is_sbe()) {
      temp_dir = temporary_directory("goblin-sbe-hset-bench");
      ring_path = temp_dir / "requests.ring";
      command.insert(command.end(), {"--ring", ring_path.string(), "1mb"});
    }
    command.insert(command.end(), config.goblin_args.begin(), config.goblin_args.end());
    command.insert(command.end(), extra_goblin_args.begin(), extra_goblin_args.end());
  } else if (spec.kind == "redis") {
    temp_dir = temporary_directory("goblin-redis-bench");
    if (fs::exists(config.parity_config)) command.push_back(config.parity_config.string());
    command.insert(command.end(), {"--bind", "127.0.0.1", "--port",
                                   std::to_string(port), "--save", "",
                                   "--appendonly", "no", "--protected-mode", "no",
                                   "--dir", temp_dir.string()});
  } else if (spec.kind == "dragonfly") {
    temp_dir = temporary_directory("goblin-dragonfly-bench");
    command.insert(command.end(), {"--bind", "127.0.0.1", "--port",
                                   std::to_string(port), "--proactor_threads=1",
                                   "--maxmemory=0", "--dir", temp_dir.string()});
  } else {
    command.insert(command.end(), {"-bind", "127.0.0.1", "-port",
                                   std::to_string(port), "-appendonly=false",
                                   "-metrics-addr="});
  }
  const bool limit_go_runtime = spec.kind == "mini-redis-go";
  ServerProcess server(spawn_server(std::move(command), limit_go_runtime,
                                    config.server_core), port,
                       std::move(temp_dir), std::move(ring_path));
  wait_for_server(server);
  return server;
}

std::unique_ptr<RespClient> open_benchmark_client(
    const EngineSpec& spec, const ServerProcess& server, double timeout_seconds) {
  if (spec.is_sbe()) {
    return std::make_unique<RespClient>(server.ring_path(), timeout_seconds);
  }
  return std::make_unique<RespClient>(server.port(), timeout_seconds);
}

std::string zero_padded(std::int64_t value, int width, int base = 10) {
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
  if (result.ec != std::errc{}) fail("number formatting failed");
  const auto length = static_cast<int>(result.ptr - buffer.data());
  if (length > width) fail("value does not fit fixed-width benchmark payload");
  return std::string(static_cast<std::size_t>(width - length), '0') +
         std::string(buffer.data(), result.ptr);
}

std::string large_field(std::int64_t item_id) {
  return "field:" + zero_padded(item_id, kRandomTokenDigits);
}

std::string small_key(std::int64_t hash_id) {
  return "h:" + zero_padded(hash_id, kRandomTokenDigits);
}

std::string small_field(int field_id) {
  return "f:" + zero_padded(field_id, 3);
}

std::string fixed_value(std::int64_t item_id, int width, char marker) {
  if (width <= 1) fail("value width must exceed marker width");
  return std::string(1, marker) + zero_padded(item_id, width - 1, 16);
}

std::vector<std::string> large_hset_command(std::int64_t start,
                                            std::int64_t count,
                                            std::string_view key, int value_bytes,
                                            char marker) {
  std::vector<std::string> command;
  command.reserve(static_cast<std::size_t>(2 + count * 2));
  command.emplace_back("HSET");
  command.emplace_back(key);
  for (std::int64_t item = start; item < start + count; ++item) {
    command.push_back(large_field(item));
    command.push_back(fixed_value(item, value_bytes, marker));
  }
  return command;
}

std::pair<std::int64_t, double> load_large(RespClient& client, std::int64_t fields,
                                           std::string_view key, int batch,
                                           int pipeline, int value_bytes,
                                           char marker) {
  const std::int64_t commands = (fields + batch - 1) / batch;
  return client.pipeline(commands, pipeline, [&](std::int64_t command_index) {
    const auto start = command_index * batch;
    const auto count = std::min<std::int64_t>(batch, fields - start);
    return large_hset_command(start, count, key, value_bytes, marker);
  });
}

std::pair<std::int64_t, double> load_small(RespClient& client, std::int64_t hashes,
                                           int fields_per_hash, int pipeline,
                                           int value_bytes) {
  return client.pipeline(hashes, pipeline, [&](std::int64_t hash_id) {
    std::vector<std::string> command;
    command.reserve(static_cast<std::size_t>(2 + fields_per_hash * 2));
    command.emplace_back("HSET");
    command.push_back(small_key(hash_id));
    for (int field = 0; field < fields_per_hash; ++field) {
      const auto ordinal = hash_id * fields_per_hash + field;
      command.push_back(small_field(field));
      command.push_back(fixed_value(ordinal, value_bytes, 'v'));
    }
    return command;
  });
}

void sleep_seconds(double seconds) {
  if (seconds > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

std::uint64_t process_resident_bytes(pid_t pid, bool use_ps = false) {
  if (use_ps) {
    const auto result = run_capture({"/bin/ps", "-o", "rss=", "-p",
                                     std::to_string(pid)}, 5.0);
    if (result.exit_code != 0) fail("ps failed while reading process RSS");
    return std::stoull(result.output) * 1024;
  }
  const fs::path status = "/proc/" + std::to_string(pid) + "/status";
  if (fs::exists(status)) {
    std::ifstream input(status);
    std::string line;
    std::uint64_t rss_kib = 0;
    std::uint64_t hugetlb_kib = 0;
    while (std::getline(input, line)) {
      std::istringstream fields(line);
      std::string name;
      std::uint64_t value = 0;
      fields >> name >> value;
      if (name == "VmRSS:") rss_kib = value;
      else if (name == "HugetlbPages:") hugetlb_kib = value;
    }
    if (rss_kib == 0) fail("process status has no VmRSS");
    return (rss_kib + hugetlb_kib) * 1024;
  }
  const auto result = run_capture({"/bin/ps", "-o", "rss=", "-p",
                                   std::to_string(pid)}, 5.0);
  if (result.exit_code != 0) fail("ps failed while reading process RSS");
  return std::stoull(result.output) * 1024;
}

std::map<std::string, std::string> info_fields(RespClient& client) {
  std::map<std::string, std::string> result;
  RespValue response;
  try {
    response = client.command({"INFO", "memory"});
  } catch (const std::exception&) {
    return result;
  }
  if (response.type != RespValue::Type::string) return result;
  std::istringstream lines(response.string);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    const auto separator = line.find(':');
    if (separator != std::string::npos) {
      result.emplace(line.substr(0, separator), line.substr(separator + 1));
    }
  }
  return result;
}

std::optional<std::int64_t> parse_int64(std::string_view text) {
  std::int64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::map<std::string, std::int64_t> goblin_memory_stats(RespClient& client,
                                                        std::string_view key) {
  std::map<std::string, std::int64_t> result;
  const auto response = client.command({"GOBLIN.MEMORY", std::string(key)});
  if (response.type != RespValue::Type::array || response.array.size() % 2 != 0) {
    return result;
  }
  for (std::size_t i = 0; i < response.array.size(); i += 2) {
    const auto& name = response.array[i];
    const auto& value = response.array[i + 1];
    if (name.type == RespValue::Type::string && value.type == RespValue::Type::string) {
      if (const auto parsed = parse_int64(value.string)) result[name.string] = *parsed;
    }
  }
  return result;
}

struct MemorySample {
  std::int64_t rss_bytes = 0;
  std::optional<std::int64_t> used_memory_bytes;
  std::optional<double> key_memory_bytes;
  std::map<std::string, std::int64_t> goblin_memory;
};

MemorySample memory_sample(RespClient& client, const ServerProcess& server,
                           const EngineSpec& spec,
                           const std::vector<std::string>& sample_keys = {}) {
  const auto info = info_fields(client);
  MemorySample sample;
  sample.rss_bytes = static_cast<std::int64_t>(
      process_resident_bytes(server.pid(), spec.kind == "mini-redis-go"));
  if (const auto found = info.find("used_memory"); found != info.end()) {
    sample.used_memory_bytes = parse_int64(found->second);
  }
  std::vector<double> reported;
  for (const auto& key : sample_keys) {
    if (spec.is_goblin()) {
      auto stats = goblin_memory_stats(client, key);
      if (const auto found = stats.find("total_allocated_bytes"); found != stats.end()) {
        reported.push_back(static_cast<double>(found->second));
      }
      if (sample.goblin_memory.empty()) sample.goblin_memory = std::move(stats);
    } else {
      try {
        const auto response = client.command({"MEMORY", "USAGE", key});
        if (response.type == RespValue::Type::integer) {
          reported.push_back(static_cast<double>(response.integer));
        }
      } catch (const std::exception&) {
        // Some RESP-compatible incumbents expose neither INFO nor MEMORY USAGE.
      }
    }
  }
  if (!reported.empty()) {
    sample.key_memory_bytes =
        std::accumulate(reported.begin(), reported.end(), 0.0) / reported.size();
  }
  return sample;
}

MemorySample warm_baseline(RespClient& client, const ServerProcess& server,
                           const EngineSpec& spec, const Config& config) {
  for (int i = 0; i < config.baseline_warmup_commands; ++i) {
    const auto response = client.command({"PING"});
    if (response.type != RespValue::Type::string || response.string != "PONG") {
      fail(spec.label + " returned an invalid PING response");
    }
  }
  const std::string key = "hsetbench:baseline-warm:" + std::to_string(::getpid()) +
                          ":" + spec.label;
  (void)load_large(client, config.baseline_warmup_fields, key, config.hset_batch,
                   config.pipeline, config.value_bytes, 'w');
  if (!client.command({"HLEN", key}).is_integer(config.baseline_warmup_fields)) {
    fail("baseline hash warmup was incomplete");
  }
  if (!client.command({"DEL", key}).is_integer(1)) fail("baseline cleanup failed");
  (void)info_fields(client);
  sleep_seconds(config.settle_seconds);
  return memory_sample(client, server, spec);
}

struct OptimizeResult {
  double seconds = 0.0;
  std::optional<std::int64_t> reclaimed_bytes;
};

OptimizeResult optimize_hash(RespClient& client, const EngineSpec& spec,
                             std::string_view key, double density) {
  if (!spec.is_goblin()) return {};
  std::ostringstream density_text;
  density_text << density;
  const auto started = Clock::now();
  const auto response = client.command({"GOBLIN.OPTIMIZE", std::string(key),
                                        density_text.str()});
  OptimizeResult result;
  result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  if (response.type == RespValue::Type::integer) result.reclaimed_bytes = response.integer;
  return result;
}

double median(std::vector<double> values) {
  if (values.empty()) fail("cannot take median of empty values");
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 != 0 ? values[middle]
                                : (values[middle - 1] + values[middle]) / 2.0;
}

std::int64_t median_int(std::vector<std::int64_t> values) {
  std::vector<double> converted(values.begin(), values.end());
  return static_cast<std::int64_t>(std::llround(median(std::move(converted))));
}

std::optional<std::int64_t> median_optional_int(
    const std::vector<std::optional<std::int64_t>>& values) {
  std::vector<std::int64_t> present;
  for (const auto value : values) if (value) present.push_back(*value);
  return present.empty() ? std::nullopt
                         : std::optional<std::int64_t>(median_int(std::move(present)));
}

std::optional<double> median_optional_double(
    const std::vector<std::optional<double>>& values) {
  std::vector<double> present;
  for (const auto value : values) if (value) present.push_back(*value);
  return present.empty() ? std::nullopt : std::optional<double>(median(std::move(present)));
}

MemorySample median_memory(const std::vector<MemorySample>& samples) {
  MemorySample result;
  std::vector<std::int64_t> rss;
  std::vector<std::optional<std::int64_t>> used;
  std::vector<std::optional<double>> keys;
  std::set<std::string> stat_names;
  for (const auto& sample : samples) {
    rss.push_back(sample.rss_bytes);
    used.push_back(sample.used_memory_bytes);
    keys.push_back(sample.key_memory_bytes);
    for (const auto& [name, value] : sample.goblin_memory) {
      (void)value;
      stat_names.insert(name);
    }
  }
  result.rss_bytes = median_int(std::move(rss));
  result.used_memory_bytes = median_optional_int(used);
  result.key_memory_bytes = median_optional_double(keys);
  for (const auto& name : stat_names) {
    std::vector<std::int64_t> values;
    for (const auto& sample : samples) {
      if (const auto found = sample.goblin_memory.find(name);
          found != sample.goblin_memory.end()) values.push_back(found->second);
    }
    if (!values.empty()) result.goblin_memory[name] = median_int(std::move(values));
  }
  return result;
}

MemorySample median_checkpoint(const std::vector<MemorySample>& samples,
                               const std::vector<MemorySample>& baselines,
                               const MemorySample& baseline) {
  if (samples.size() != baselines.size()) fail("memory samples are not paired");
  MemorySample result = median_memory(samples);
  std::vector<std::int64_t> rss_deltas;
  std::vector<std::int64_t> used_deltas;
  for (std::size_t i = 0; i < samples.size(); ++i) {
    rss_deltas.push_back(samples[i].rss_bytes - baselines[i].rss_bytes);
    if (samples[i].used_memory_bytes && baselines[i].used_memory_bytes) {
      used_deltas.push_back(*samples[i].used_memory_bytes -
                            *baselines[i].used_memory_bytes);
    }
  }
  result.rss_bytes = baseline.rss_bytes + median_int(std::move(rss_deltas));
  if (baseline.used_memory_bytes && !used_deltas.empty()) {
    result.used_memory_bytes = *baseline.used_memory_bytes + median_int(std::move(used_deltas));
  }
  return result;
}

OptimizeResult median_optimize(const std::vector<OptimizeResult>& results) {
  std::vector<double> seconds;
  std::vector<std::optional<std::int64_t>> reclaimed;
  for (const auto& result : results) {
    seconds.push_back(result.seconds);
    reclaimed.push_back(result.reclaimed_bytes);
  }
  return {median(std::move(seconds)), median_optional_int(reclaimed)};
}

double redis_benchmark_rps(const Config& config, const ServerProcess& server,
                           const std::vector<std::string>& command, int requests,
                           std::int64_t keyspace) {
  std::vector<std::string> arguments{config.redis_benchmark.string(), "-h", "127.0.0.1",
                                     "-p", std::to_string(server.port()), "-n",
                                     std::to_string(requests), "-P",
                                     std::to_string(config.pipeline), "-c", "1", "-q",
                                     "-r", std::to_string(std::max<std::int64_t>(1, keyspace))};
  arguments.insert(arguments.end(), command.begin(), command.end());
  const auto result = run_capture(std::move(arguments), config.timeout_seconds);
  const std::regex throughput(R"(([0-9]+\.?[0-9]*)\s+requests per second)");
  std::smatch match;
  if (result.exit_code != 0 || !std::regex_search(result.output, match, throughput)) {
    fail("redis-benchmark produced no throughput line: " + result.output);
  }
  return std::stod(match[1].str());
}

double benchmark_point(const Config& config, const ServerProcess& server,
                       const EngineSpec& spec, RespClient& client,
                       const std::vector<std::string>& command,
                       std::int64_t keyspace) {
  if (spec.is_sbe()) {
    if (config.warmup_requests > 0) {
      (void)client.benchmark_sbe_point(
          command, config.warmup_requests, keyspace, config.seed);
    }
    std::vector<double> rates;
    for (int round = 0; round < config.rounds; ++round) {
      rates.push_back(client.benchmark_sbe_point(
          command, config.requests, keyspace,
          config.seed + static_cast<std::uint64_t>(round + 1)));
    }
    return median(std::move(rates));
  }
  if (config.warmup_requests > 0) {
    (void)redis_benchmark_rps(config, server, command, config.warmup_requests, keyspace);
  }
  std::vector<double> rates;
  for (int round = 0; round < config.rounds; ++round) {
    rates.push_back(redis_benchmark_rps(config, server, command, config.requests, keyspace));
  }
  return median(std::move(rates));
}

void verify_large_hash(RespClient& client, std::string_view key, std::int64_t fields,
                       int value_bytes, char marker) {
  if (!client.command({"HLEN", std::string(key)}).is_integer(fields)) {
    fail("large hash field count is incorrect");
  }
  for (const auto item : std::array<std::int64_t, 3>{0, fields / 2, fields - 1}) {
    const auto response = client.command({"HGET", std::string(key), large_field(item)});
    if (response.type != RespValue::Type::string ||
        response.string != fixed_value(item, value_bytes, marker)) {
      fail("large hash value verification failed");
    }
  }
}

struct LargeHashResult {
  EngineSpec engine;
  std::int64_t fields = 0;
  int field_bytes = 0;
  int initial_value_bytes = 0;
  int grown_value_bytes = 0;
  std::int64_t load_commands = 0;
  double load_seconds = 0.0;
  double load_fields_per_second = 0.0;
  double update_hset_operations_per_second = 0.0;
  std::int64_t grow_commands = 0;
  double grow_seconds = 0.0;
  double grow_fields_per_second = 0.0;
  MemorySample baseline;
  MemorySample loaded_before_optimize;
  MemorySample loaded_after_optimize;
  MemorySample after_same_width_updates;
  MemorySample grown_before_optimize;
  MemorySample grown_after_optimize;
  OptimizeResult initial_optimize;
  OptimizeResult grown_optimize;
  int construction_rounds = 1;
};

struct SmallHashResult {
  EngineSpec engine;
  std::int64_t hashes = 0;
  int fields_per_hash = 0;
  std::int64_t total_fields = 0;
  int key_bytes = 0;
  int field_bytes = 0;
  int value_bytes = 0;
  std::int64_t load_commands = 0;
  double load_seconds = 0.0;
  double load_hashes_per_second = 0.0;
  double load_fields_per_second = 0.0;
  double update_hset_operations_per_second = 0.0;
  MemorySample baseline;
  MemorySample loaded;
  MemorySample after_updates;
  int construction_rounds = 1;
};

struct RelocationResult {
  EngineSpec engine;
  std::string pattern;
  double density = 0.0;
  std::int64_t fields = 0;
  std::int64_t grown_fields = 0;
  double load_seconds = 0.0;
  double load_fields_per_second = 0.0;
  std::optional<double> first_growth_latency_us;
  double remaining_growth_seconds = 0.0;
  double hget_operations_per_second = 0.0;
  MemorySample baseline;
  MemorySample loaded;
  MemorySample grown_before_optimize;
  MemorySample grown_after_optimize;
  OptimizeResult optimize;
  int rounds = 1;
};

struct LatencyStats {
  double minimum = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double maximum = 0.0;
};

struct MixedHashResult {
  EngineSpec engine;
  int initial_fields = 0;
  int samples = 0;
  int warmup = 0;
  std::array<std::int64_t, 4> operations{};
  std::array<LatencyStats, 4> operation_latency_us{};
  double load_seconds = 0.0;
  double operations_per_second = 0.0;
  LatencyStats latency_us;
  MemorySample baseline;
  MemorySample loaded;
  MemorySample after_mixed;
  int final_fields = 0;
  int rounds = 1;
};

LargeHashResult run_large_once(const EngineSpec& spec, const Config& config) {
  auto server = start_engine(spec, config);
  auto client_storage = open_benchmark_client(spec, server, config.timeout_seconds);
  auto& client = *client_storage;
  const std::string key = "hsetbench:large:" + std::to_string(::getpid()) + ":" + spec.label;
  LargeHashResult result;
  result.engine = spec;
  result.fields = config.large_fields;
  result.field_bytes = static_cast<int>(large_field(0).size());
  result.initial_value_bytes = config.value_bytes;
  result.grown_value_bytes = config.grown_value_bytes;
  result.baseline = warm_baseline(client, server, spec, config);

  std::tie(result.load_commands, result.load_seconds) =
      load_large(client, config.large_fields, key, config.hset_batch,
                 config.pipeline, config.value_bytes, 'v');
  result.load_fields_per_second = config.large_fields / result.load_seconds;
  verify_large_hash(client, key, config.large_fields, config.value_bytes, 'v');
  sleep_seconds(config.settle_seconds);
  result.loaded_before_optimize = memory_sample(client, server, spec, {key});

  result.initial_optimize = optimize_hash(client, spec, key, config.optimize_density);
  sleep_seconds(config.settle_seconds);
  result.loaded_after_optimize = memory_sample(client, server, spec, {key});

  result.update_hset_operations_per_second = benchmark_point(
      config, server, spec, client,
      {"HSET", key, "field:__rand_int__",
       std::string(static_cast<std::size_t>(config.value_bytes), 'u')},
      config.large_fields);
  if (!client.command({"HLEN", key}).is_integer(config.large_fields)) {
    fail("random existing-field HSET created fields");
  }
  sleep_seconds(config.settle_seconds);
  result.after_same_width_updates = memory_sample(client, server, spec, {key});

  std::tie(result.grow_commands, result.grow_seconds) =
      load_large(client, config.large_fields, key, config.hset_batch,
                 config.pipeline, config.grown_value_bytes, 'g');
  result.grow_fields_per_second = config.large_fields / result.grow_seconds;
  verify_large_hash(client, key, config.large_fields, config.grown_value_bytes, 'g');
  sleep_seconds(config.settle_seconds);
  result.grown_before_optimize = memory_sample(client, server, spec, {key});

  result.grown_optimize = optimize_hash(client, spec, key, config.optimize_density);
  sleep_seconds(config.settle_seconds);
  result.grown_after_optimize = memory_sample(client, server, spec, {key});
  return result;
}

template <typename Result, typename Member>
std::vector<MemorySample> memory_members(const std::vector<Result>& results, Member member) {
  std::vector<MemorySample> values;
  values.reserve(results.size());
  for (const auto& result : results) values.push_back(result.*member);
  return values;
}

LargeHashResult median_large(const std::vector<LargeHashResult>& results) {
  LargeHashResult output = results.front();
  const auto baselines = memory_members(results, &LargeHashResult::baseline);
  output.baseline = median_memory(baselines);
  auto doubles = [&](auto member) {
    std::vector<double> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median(std::move(values));
  };
  auto integers = [&](auto member) {
    std::vector<std::int64_t> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median_int(std::move(values));
  };
  output.load_commands = integers(&LargeHashResult::load_commands);
  output.load_seconds = doubles(&LargeHashResult::load_seconds);
  output.load_fields_per_second = doubles(&LargeHashResult::load_fields_per_second);
  output.update_hset_operations_per_second =
      doubles(&LargeHashResult::update_hset_operations_per_second);
  output.grow_commands = integers(&LargeHashResult::grow_commands);
  output.grow_seconds = doubles(&LargeHashResult::grow_seconds);
  output.grow_fields_per_second = doubles(&LargeHashResult::grow_fields_per_second);
  output.loaded_before_optimize = median_checkpoint(
      memory_members(results, &LargeHashResult::loaded_before_optimize), baselines,
      output.baseline);
  output.loaded_after_optimize = median_checkpoint(
      memory_members(results, &LargeHashResult::loaded_after_optimize), baselines,
      output.baseline);
  output.after_same_width_updates = median_checkpoint(
      memory_members(results, &LargeHashResult::after_same_width_updates), baselines,
      output.baseline);
  output.grown_before_optimize = median_checkpoint(
      memory_members(results, &LargeHashResult::grown_before_optimize), baselines,
      output.baseline);
  output.grown_after_optimize = median_checkpoint(
      memory_members(results, &LargeHashResult::grown_after_optimize), baselines,
      output.baseline);
  std::vector<OptimizeResult> initial;
  std::vector<OptimizeResult> grown;
  for (const auto& result : results) {
    initial.push_back(result.initial_optimize);
    grown.push_back(result.grown_optimize);
  }
  output.initial_optimize = median_optimize(initial);
  output.grown_optimize = median_optimize(grown);
  output.construction_rounds = static_cast<int>(results.size());
  return output;
}

LargeHashResult run_large(const EngineSpec& spec, const Config& config) {
  std::vector<LargeHashResult> rounds;
  for (int round = 0; round < config.construction_rounds; ++round) {
    rounds.push_back(run_large_once(spec, config));
  }
  return median_large(rounds);
}

void verify_small_hashes(RespClient& client, std::int64_t hashes,
                         int fields_per_hash, int value_bytes,
                         bool verify_middle_update = false) {
  for (const auto hash_id : std::array<std::int64_t, 3>{0, hashes / 2, hashes - 1}) {
    const auto key = small_key(hash_id);
    if (!client.command({"HLEN", key}).is_integer(fields_per_hash)) {
      fail("small hash field count is incorrect");
    }
    const auto ordinal = hash_id * fields_per_hash + fields_per_hash - 1;
    const auto response = client.command({"HGET", key, small_field(fields_per_hash - 1)});
    if (response.type != RespValue::Type::string ||
        response.string != fixed_value(ordinal, value_bytes, 'v')) {
      fail("small hash value verification failed");
    }
    if (verify_middle_update) {
      const auto middle = client.command(
          {"HGET", key, small_field(fields_per_hash / 2)});
      if (middle.type != RespValue::Type::string ||
          middle.string != std::string(static_cast<std::size_t>(value_bytes), 'u')) {
        fail("small hash point probe did not update the intended keyspace");
      }
    }
  }
}

SmallHashResult run_small_once(const EngineSpec& spec, const Config& config,
                               int fields_per_hash) {
  auto server = start_engine(spec, config);
  auto client_storage = open_benchmark_client(spec, server, config.timeout_seconds);
  auto& client = *client_storage;
  SmallHashResult result;
  result.engine = spec;
  result.hashes = std::max<std::int64_t>(1, config.small_total_fields / fields_per_hash);
  result.fields_per_hash = fields_per_hash;
  result.total_fields = result.hashes * fields_per_hash;
  result.key_bytes = static_cast<int>(small_key(0).size());
  result.field_bytes = static_cast<int>(small_field(0).size());
  result.value_bytes = config.value_bytes;
  result.baseline = warm_baseline(client, server, spec, config);
  std::tie(result.load_commands, result.load_seconds) =
      load_small(client, result.hashes, fields_per_hash, config.pipeline,
                 config.value_bytes);
  result.load_hashes_per_second = result.hashes / result.load_seconds;
  result.load_fields_per_second = result.total_fields / result.load_seconds;
  verify_small_hashes(client, result.hashes, fields_per_hash, config.value_bytes);
  const std::vector<std::string> sample_keys{small_key(0), small_key(result.hashes / 2),
                                             small_key(result.hashes - 1)};
  sleep_seconds(config.settle_seconds);
  result.loaded = memory_sample(client, server, spec, sample_keys);
  result.update_hset_operations_per_second = benchmark_point(
      config, server, spec, client,
      {"HSET", "h:__rand_int__", small_field(fields_per_hash / 2),
       std::string(static_cast<std::size_t>(config.value_bytes), 'u')},
      result.hashes);
  verify_small_hashes(client, result.hashes, fields_per_hash, config.value_bytes,
                      /*verify_middle_update=*/true);
  sleep_seconds(config.settle_seconds);
  result.after_updates = memory_sample(client, server, spec, sample_keys);
  return result;
}

SmallHashResult median_small(const std::vector<SmallHashResult>& results) {
  SmallHashResult output = results.front();
  const auto baselines = memory_members(results, &SmallHashResult::baseline);
  output.baseline = median_memory(baselines);
  auto doubles = [&](auto member) {
    std::vector<double> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median(std::move(values));
  };
  auto integers = [&](auto member) {
    std::vector<std::int64_t> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median_int(std::move(values));
  };
  output.load_commands = integers(&SmallHashResult::load_commands);
  output.load_seconds = doubles(&SmallHashResult::load_seconds);
  output.load_hashes_per_second = doubles(&SmallHashResult::load_hashes_per_second);
  output.load_fields_per_second = doubles(&SmallHashResult::load_fields_per_second);
  output.update_hset_operations_per_second =
      doubles(&SmallHashResult::update_hset_operations_per_second);
  output.loaded = median_checkpoint(memory_members(results, &SmallHashResult::loaded),
                                    baselines, output.baseline);
  output.after_updates = median_checkpoint(
      memory_members(results, &SmallHashResult::after_updates), baselines,
      output.baseline);
  output.construction_rounds = static_cast<int>(results.size());
  return output;
}

SmallHashResult run_small(const EngineSpec& spec, const Config& config,
                          int fields_per_hash) {
  std::vector<SmallHashResult> rounds;
  for (int round = 0; round < config.construction_rounds; ++round) {
    rounds.push_back(run_small_once(spec, config, fields_per_hash));
  }
  return median_small(rounds);
}

std::vector<std::int64_t> relocation_ids(std::int64_t fields, std::int64_t count,
                                         std::string_view pattern,
                                         std::uint64_t seed) {
  if (count == 0) return {};
  if (pattern == "clustered") {
    std::vector<std::int64_t> ids(static_cast<std::size_t>(count));
    const auto start = (fields - count) / 2;
    std::iota(ids.begin(), ids.end(), start);
    return ids;
  }
  std::vector<std::int64_t> ids(static_cast<std::size_t>(fields));
  std::iota(ids.begin(), ids.end(), 0);
  std::mt19937_64 random(seed);
  std::shuffle(ids.begin(), ids.end(), random);
  ids.resize(static_cast<std::size_t>(count));
  return ids;
}

void verify_relocation(RespClient& client, std::string_view key,
                       std::int64_t fields,
                       const std::vector<std::int64_t>& grown_ids,
                       int initial_value_bytes, int grown_value_bytes) {
  if (!client.command({"HLEN", std::string(key)}).is_integer(fields)) {
    fail("relocation workload changed the hash field count");
  }
  std::vector<std::uint8_t> grown(static_cast<std::size_t>(fields), 0);
  for (const auto id : grown_ids) grown[static_cast<std::size_t>(id)] = 1;
  if (!grown_ids.empty()) {
    for (const auto id : std::array<std::int64_t, 3>{
             grown_ids.front(), grown_ids[grown_ids.size() / 2], grown_ids.back()}) {
      const auto response = client.command({"HGET", std::string(key), large_field(id)});
      if (response.type != RespValue::Type::string ||
          response.string != fixed_value(id, grown_value_bytes, 'g')) {
        fail("relocated hash value verification failed");
      }
    }
  }
  int checked = 0;
  for (std::int64_t id = 0; id < fields && checked < 3; ++id) {
    if (grown[static_cast<std::size_t>(id)] != 0) continue;
    const auto response = client.command({"HGET", std::string(key), large_field(id)});
    if (response.type != RespValue::Type::string ||
        response.string != fixed_value(id, initial_value_bytes, 'v')) {
      fail("unrelocated hash value verification failed");
    }
    ++checked;
  }
}

RelocationResult run_relocation_once(const EngineSpec& spec, const Config& config,
                                     std::string_view pattern, double density,
                                     int round_index) {
  auto server = start_engine(spec, config, {"--hash-listpack-max-entries", "0"});
  auto client_storage = open_benchmark_client(spec, server, config.timeout_seconds);
  auto& client = *client_storage;
  std::ostringstream density_text;
  density_text << density;
  const std::string key = "hsetbench:relocation:" + std::to_string(::getpid()) +
                          ":" + std::string(pattern) + ":" + density_text.str() +
                          ":" + std::to_string(round_index);
  RelocationResult result;
  result.engine = spec;
  result.pattern = pattern;
  result.density = density;
  result.fields = config.relocation_fields;
  result.baseline = warm_baseline(client, server, spec, config);
  std::int64_t ignored_commands = 0;
  std::tie(ignored_commands, result.load_seconds) =
      load_large(client, config.relocation_fields, key, config.hset_batch,
                 config.pipeline, config.value_bytes, 'v');
  (void)ignored_commands;
  result.load_fields_per_second = config.relocation_fields / result.load_seconds;
  verify_large_hash(client, key, config.relocation_fields, config.value_bytes, 'v');
  sleep_seconds(config.settle_seconds);
  result.loaded = memory_sample(client, server, spec, {key});

  result.grown_fields = static_cast<std::int64_t>(
      std::llround(static_cast<double>(config.relocation_fields) * density));
  result.grown_fields = std::clamp<std::int64_t>(result.grown_fields, 0,
                                                 config.relocation_fields);
  const auto ids = relocation_ids(config.relocation_fields, result.grown_fields,
                                  pattern, config.seed + round_index);
  if (!ids.empty()) {
    const auto id = ids.front();
    const auto started = Clock::now();
    const auto response = client.command({"HSET", key, large_field(id),
                                          fixed_value(id, config.grown_value_bytes, 'g')});
    result.first_growth_latency_us =
        std::chrono::duration<double, std::micro>(Clock::now() - started).count();
    if (!response.is_integer(0)) fail("first relocation HSET inserted a field");
    if (ids.size() > 1) {
      const auto remaining = client.pipeline(
          static_cast<std::int64_t>(ids.size() - 1), config.pipeline,
          [&](std::int64_t index) {
            const auto grow_id = ids[static_cast<std::size_t>(index + 1)];
            return std::vector<std::string>{
                "HSET", key, large_field(grow_id),
                fixed_value(grow_id, config.grown_value_bytes, 'g')};
          });
      result.remaining_growth_seconds = remaining.second;
    }
  }
  verify_relocation(client, key, config.relocation_fields, ids,
                    config.value_bytes, config.grown_value_bytes);
  sleep_seconds(config.settle_seconds);
  result.grown_before_optimize = memory_sample(client, server, spec, {key});
  result.hget_operations_per_second = benchmark_point(
      config, server, spec, client, {"HGET", key, "field:__rand_int__"},
      config.relocation_fields);
  result.optimize = optimize_hash(client, spec, key, config.optimize_density);
  verify_relocation(client, key, config.relocation_fields, ids,
                    config.value_bytes, config.grown_value_bytes);
  sleep_seconds(config.settle_seconds);
  result.grown_after_optimize = memory_sample(client, server, spec, {key});
  return result;
}

RelocationResult median_relocation(const std::vector<RelocationResult>& results) {
  RelocationResult output = results.front();
  const auto baselines = memory_members(results, &RelocationResult::baseline);
  output.baseline = median_memory(baselines);
  auto doubles = [&](auto member) {
    std::vector<double> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median(std::move(values));
  };
  output.load_seconds = doubles(&RelocationResult::load_seconds);
  output.load_fields_per_second = doubles(&RelocationResult::load_fields_per_second);
  output.remaining_growth_seconds = doubles(&RelocationResult::remaining_growth_seconds);
  output.hget_operations_per_second =
      doubles(&RelocationResult::hget_operations_per_second);
  std::vector<std::optional<double>> first_growth;
  std::vector<OptimizeResult> optimizes;
  for (const auto& result : results) {
    first_growth.push_back(result.first_growth_latency_us);
    optimizes.push_back(result.optimize);
  }
  output.first_growth_latency_us = median_optional_double(first_growth);
  output.loaded = median_checkpoint(memory_members(results, &RelocationResult::loaded),
                                    baselines, output.baseline);
  output.grown_before_optimize = median_checkpoint(
      memory_members(results, &RelocationResult::grown_before_optimize), baselines,
      output.baseline);
  output.grown_after_optimize = median_checkpoint(
      memory_members(results, &RelocationResult::grown_after_optimize), baselines,
      output.baseline);
  output.optimize = median_optimize(optimizes);
  output.rounds = static_cast<int>(results.size());
  return output;
}

RelocationResult run_relocation(const EngineSpec& spec, const Config& config,
                                std::string_view pattern, double density) {
  std::vector<RelocationResult> rounds;
  for (int round = 0; round < config.relocation_rounds; ++round) {
    rounds.push_back(run_relocation_once(spec, config, pattern, density, round));
  }
  return median_relocation(rounds);
}

void dense_append(std::vector<std::int64_t>& values, std::vector<std::int64_t>& positions,
                  std::int64_t value) {
  if (static_cast<std::size_t>(value) >= positions.size()) {
    positions.resize(static_cast<std::size_t>(value + 1), -1);
  }
  positions[static_cast<std::size_t>(value)] = static_cast<std::int64_t>(values.size());
  values.push_back(value);
}

void dense_remove(std::vector<std::int64_t>& values,
                  std::vector<std::int64_t>& positions, std::int64_t value) {
  const auto position = positions.at(static_cast<std::size_t>(value));
  if (position < 0) fail("mixed workload model removed an absent value");
  const auto last = values.back();
  values.pop_back();
  positions[static_cast<std::size_t>(value)] = -1;
  if (static_cast<std::size_t>(position) < values.size()) {
    values[static_cast<std::size_t>(position)] = last;
    positions[static_cast<std::size_t>(last)] = position;
  }
}

struct MixedCommand {
  std::uint8_t operation = 0;  // insert, update, delete, grow
  std::int64_t item_id = 0;
  int value_bytes = 0;
  char marker = 'v';
  std::int64_t expected = 0;
};

struct MixedModel {
  std::vector<MixedCommand> commands;
  int final_fields = 0;
};

MixedModel build_mixed_model(const Config& config) {
  const int total_commands = config.mixed_warmup + config.mixed_samples;
  const std::size_t maximum_ids =
      static_cast<std::size_t>(config.mixed_fields + total_commands + 1);
  std::vector<std::int64_t> live;
  std::vector<std::int64_t> short_values;
  live.reserve(maximum_ids);
  short_values.reserve(maximum_ids);
  std::vector<std::int64_t> live_positions(maximum_ids, -1);
  std::vector<std::int64_t> short_positions(maximum_ids, -1);
  std::vector<std::uint8_t> grown(maximum_ids, 0);
  for (int id = 0; id < config.mixed_fields; ++id) {
    dense_append(live, live_positions, id);
    dense_append(short_values, short_positions, id);
  }
  std::mt19937_64 random(config.seed);
  const int total_weight = std::accumulate(config.mixed_weights.begin(),
                                           config.mixed_weights.end(), 0);
  std::uniform_int_distribution<int> choose_weight(1, total_weight);
  std::int64_t next_id = config.mixed_fields;
  MixedModel model;
  model.commands.reserve(static_cast<std::size_t>(total_commands));
  for (int command_index = 0; command_index < total_commands; ++command_index) {
    int choice = choose_weight(random);
    int operation = 0;
    while (operation < 3 && choice > config.mixed_weights[operation]) {
      choice -= config.mixed_weights[operation++];
    }
    if (operation == 2 && live.size() <= 1) operation = 0;
    if (operation == 3 && short_values.empty()) operation = 1;
    MixedCommand command;
    command.operation = static_cast<std::uint8_t>(operation);
    if (operation == 0) {
      command.item_id = next_id++;
      dense_append(live, live_positions, command.item_id);
      dense_append(short_values, short_positions, command.item_id);
      command.value_bytes = config.value_bytes;
      command.marker = 'v';
      command.expected = 1;
    } else if (operation == 2) {
      std::uniform_int_distribution<std::size_t> choose(0, live.size() - 1);
      command.item_id = live[choose(random)];
      dense_remove(live, live_positions, command.item_id);
      if (short_positions[static_cast<std::size_t>(command.item_id)] >= 0) {
        dense_remove(short_values, short_positions, command.item_id);
      }
      grown[static_cast<std::size_t>(command.item_id)] = 0;
      command.expected = 1;
    } else if (operation == 3) {
      std::uniform_int_distribution<std::size_t> choose(0, short_values.size() - 1);
      command.item_id = short_values[choose(random)];
      dense_remove(short_values, short_positions, command.item_id);
      grown[static_cast<std::size_t>(command.item_id)] = 1;
      command.value_bytes = config.grown_value_bytes;
      command.marker = 'g';
    } else {
      std::uniform_int_distribution<std::size_t> choose(0, live.size() - 1);
      command.item_id = live[choose(random)];
      command.value_bytes = grown[static_cast<std::size_t>(command.item_id)] != 0
                                ? config.grown_value_bytes
                                : config.value_bytes;
      command.marker = 'u';
    }
    model.commands.push_back(command);
  }
  model.final_fields = static_cast<int>(live.size());
  return model;
}

std::vector<std::string> mixed_wire_command(std::string_view key,
                                            const MixedCommand& command) {
  if (command.operation == 2) {
    return {"HDEL", std::string(key), large_field(command.item_id)};
  }
  return {"HSET", std::string(key), large_field(command.item_id),
          fixed_value(command.item_id, command.value_bytes, command.marker)};
}

LatencyStats latency_stats(std::vector<double> values) {
  if (values.empty()) return {};
  std::sort(values.begin(), values.end());
  auto percentile = [&](double fraction) {
    const auto index = static_cast<std::size_t>(
        std::llround(static_cast<double>(values.size() - 1) * fraction));
    return values[index];
  };
  return {values.front(), percentile(0.50), percentile(0.95), percentile(0.99),
          percentile(0.999), values.back()};
}

MixedHashResult run_mixed_once(const EngineSpec& spec, const Config& config,
                               int round_index) {
  auto server = start_engine(spec, config);
  auto client_storage = open_benchmark_client(spec, server, config.timeout_seconds);
  auto& client = *client_storage;
  const std::string key = "hsetbench:mixed:" + std::to_string(::getpid()) + ":" +
                          spec.label + ":" + std::to_string(round_index);
  MixedHashResult result;
  result.engine = spec;
  result.initial_fields = config.mixed_fields;
  result.samples = config.mixed_samples;
  result.warmup = config.mixed_warmup;
  result.baseline = warm_baseline(client, server, spec, config);
  std::int64_t ignored = 0;
  std::tie(ignored, result.load_seconds) =
      load_large(client, config.mixed_fields, key, config.hset_batch,
                 config.pipeline, config.value_bytes, 'v');
  (void)ignored;
  sleep_seconds(config.settle_seconds);
  result.loaded = memory_sample(client, server, spec, {key});

  const auto model = build_mixed_model(config);
  result.final_fields = model.final_fields;
  for (int index = 0; index < config.mixed_warmup; ++index) {
    const auto& command = model.commands[static_cast<std::size_t>(index)];
    const auto response = client.command(mixed_wire_command(key, command));
    if (!response.is_integer(command.expected)) fail("mixed warmup reply mismatch");
  }
  std::vector<double> all_latencies;
  std::array<std::vector<double>, 4> operation_latencies;
  all_latencies.reserve(static_cast<std::size_t>(config.mixed_samples));
  const auto started = Clock::now();
  for (int index = config.mixed_warmup;
       index < config.mixed_warmup + config.mixed_samples; ++index) {
    const auto& command = model.commands[static_cast<std::size_t>(index)];
    const auto command_started = Clock::now();
    const auto response = client.command(mixed_wire_command(key, command));
    const double latency =
        std::chrono::duration<double, std::micro>(Clock::now() - command_started).count();
    if (!response.is_integer(command.expected)) fail("mixed workload reply mismatch");
    all_latencies.push_back(latency);
    operation_latencies[command.operation].push_back(latency);
    ++result.operations[command.operation];
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
  result.operations_per_second = config.mixed_samples / seconds;
  if (!client.command({"HLEN", key}).is_integer(result.final_fields)) {
    fail("mixed workload final HLEN did not match model");
  }
  sleep_seconds(config.settle_seconds);
  result.after_mixed = memory_sample(client, server, spec, {key});
  result.latency_us = latency_stats(std::move(all_latencies));
  for (std::size_t operation = 0; operation < operation_latencies.size(); ++operation) {
    result.operation_latency_us[operation] =
        latency_stats(std::move(operation_latencies[operation]));
  }
  return result;
}

LatencyStats median_latency(const std::vector<LatencyStats>& values) {
  auto field = [&](auto member) {
    std::vector<double> samples;
    for (const auto& value : values) samples.push_back(value.*member);
    return median(std::move(samples));
  };
  return {field(&LatencyStats::minimum), field(&LatencyStats::p50),
          field(&LatencyStats::p95), field(&LatencyStats::p99),
          field(&LatencyStats::p999), field(&LatencyStats::maximum)};
}

MixedHashResult median_mixed(const std::vector<MixedHashResult>& results) {
  MixedHashResult output = results.front();
  const auto baselines = memory_members(results, &MixedHashResult::baseline);
  output.baseline = median_memory(baselines);
  auto doubles = [&](auto member) {
    std::vector<double> values;
    for (const auto& result : results) values.push_back(result.*member);
    return median(std::move(values));
  };
  for (const auto& result : results) {
    if (result.operations != output.operations || result.final_fields != output.final_fields) {
      fail("mixed rounds did not use the same deterministic workload");
    }
  }
  output.load_seconds = doubles(&MixedHashResult::load_seconds);
  output.operations_per_second = doubles(&MixedHashResult::operations_per_second);
  std::vector<LatencyStats> overall;
  for (const auto& result : results) overall.push_back(result.latency_us);
  output.latency_us = median_latency(overall);
  for (std::size_t operation = 0; operation < output.operation_latency_us.size();
       ++operation) {
    std::vector<LatencyStats> samples;
    for (const auto& result : results) {
      samples.push_back(result.operation_latency_us[operation]);
    }
    output.operation_latency_us[operation] = median_latency(samples);
  }
  output.loaded = median_checkpoint(memory_members(results, &MixedHashResult::loaded),
                                    baselines, output.baseline);
  output.after_mixed = median_checkpoint(
      memory_members(results, &MixedHashResult::after_mixed), baselines,
      output.baseline);
  output.rounds = static_cast<int>(results.size());
  return output;
}

MixedHashResult run_mixed(const EngineSpec& spec, const Config& config) {
  std::vector<MixedHashResult> rounds;
  for (int round = 0; round < config.mixed_rounds; ++round) {
    rounds.push_back(run_mixed_once(spec, config, round));
  }
  return median_mixed(rounds);
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

std::string grouped(std::int64_t value) {
  const bool negative = value < 0;
  std::string digits = std::to_string(negative ? -value : value);
  for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(digits.size()) - 3;
       position > 0; position -= 3) {
    digits.insert(static_cast<std::size_t>(position), 1, ',');
  }
  return negative ? "-" + digits : digits;
}

std::string fixed(double value, int digits = 2) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(digits) << value;
  return output.str();
}

std::string optional_fixed(const std::optional<double>& value, int digits = 2) {
  return value ? fixed(*value, digits) : "n/a";
}

std::optional<std::int64_t> memory_delta(
    const std::optional<std::int64_t>& after,
    const std::optional<std::int64_t>& before) {
  return after && before ? std::optional<std::int64_t>(*after - *before) : std::nullopt;
}

std::optional<double> memory_per(const std::optional<std::int64_t>& value,
                                 std::int64_t count) {
  return value ? std::optional<double>(static_cast<double>(*value) / count) : std::nullopt;
}

std::int64_t goblin_stat(const MemorySample& sample, std::string_view name) {
  const auto found = sample.goblin_memory.find(std::string(name));
  return found == sample.goblin_memory.end() ? 0 : found->second;
}

std::string json_escape(std::string_view text) {
  std::string output;
  output.reserve(text.size() + 2);
  output.push_back('"');
  for (const char character : text) {
    switch (character) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output.push_back(character); break;
    }
  }
  output.push_back('"');
  return output;
}

void json_optional(std::ostream& output, const std::optional<std::int64_t>& value) {
  if (value) output << *value;
  else output << "null";
}

void json_optional(std::ostream& output, const std::optional<double>& value) {
  if (value) output << std::setprecision(17) << *value;
  else output << "null";
}

void json_memory(std::ostream& output, const MemorySample& sample, int indent) {
  const std::string prefix(static_cast<std::size_t>(indent), ' ');
  output << "{\n" << prefix << "  \"rss_bytes\": " << sample.rss_bytes
         << ",\n" << prefix << "  \"used_memory_bytes\": ";
  json_optional(output, sample.used_memory_bytes);
  output << ",\n" << prefix << "  \"key_memory_bytes\": ";
  json_optional(output, sample.key_memory_bytes);
  output << ",\n" << prefix << "  \"goblin_memory\": ";
  if (sample.goblin_memory.empty()) {
    output << "null\n";
  } else {
    output << "{\n";
    std::size_t index = 0;
    for (const auto& [name, value] : sample.goblin_memory) {
      output << prefix << "    " << json_escape(name) << ": " << value;
      if (++index != sample.goblin_memory.size()) output << ',';
      output << '\n';
    }
    output << prefix << "  }\n";
  }
  output << prefix << '}';
}

void json_engine(std::ostream& output, const EngineSpec& engine) {
  output << "\"engine\": " << json_escape(engine.label)
         << ", \"kind\": " << json_escape(engine.kind);
}

struct BenchmarkResults {
  std::string generated_at;
  std::vector<LargeHashResult> large;
  std::vector<SmallHashResult> small;
  std::vector<RelocationResult> relocation;
  std::vector<MixedHashResult> mixed;
};

void write_json(const Config& config, const BenchmarkResults& results) {
  if (!config.output_json.parent_path().empty()) {
    fs::create_directories(config.output_json.parent_path());
  }
  std::ofstream output(config.output_json);
  if (!output) fail("cannot open JSON output");
  output << "{\n  \"generated_at\": " << json_escape(results.generated_at)
         << ",\n  \"config\": {\n"
         << "    \"rss_source\": \"ps -o rss= for mini-redis-go; /proc/<pid>/status VmRSS + HugetlbPages otherwise\",\n"
         << "    \"client\": \"native C++ RESP/TCP or typed SBE/shared-memory-ring harness\",\n"
         << "    \"large_fields\": " << config.large_fields << ",\n"
         << "    \"small_total_fields\": " << config.small_total_fields << ",\n"
         << "    \"value_bytes\": " << config.value_bytes << ",\n"
         << "    \"grown_value_bytes\": " << config.grown_value_bytes << ",\n"
         << "    \"hset_batch\": " << config.hset_batch << ",\n"
         << "    \"pipeline\": " << config.pipeline << ",\n"
         << "    \"server_core\": " << config.server_core << ",\n"
         << "    \"client_core\": " << config.client_core << ",\n"
         << "    \"requests\": " << config.requests << ",\n"
         << "    \"rounds\": " << config.rounds << ",\n"
         << "    \"construction_rounds\": " << config.construction_rounds << ",\n"
         << "    \"relocation_fields\": " << config.relocation_fields << ",\n"
         << "    \"relocation_rounds\": " << config.relocation_rounds << ",\n"
         << "    \"mixed_fields\": " << config.mixed_fields << ",\n"
         << "    \"mixed_samples\": " << config.mixed_samples << ",\n"
         << "    \"mixed_warmup\": " << config.mixed_warmup << ",\n"
         << "    \"mixed_rounds\": " << config.mixed_rounds << ",\n"
         << "    \"seed\": " << config.seed << "\n  },\n";

  output << "  \"large_hash\": [\n";
  for (std::size_t i = 0; i < results.large.size(); ++i) {
    const auto& result = results.large[i];
    output << "    {";
    json_engine(output, result.engine);
    output << ", \"fields\": " << result.fields
           << ", \"load_seconds\": " << std::setprecision(17) << result.load_seconds
           << ", \"load_fields_per_second\": " << result.load_fields_per_second
           << ", \"update_hset_operations_per_second\": "
           << result.update_hset_operations_per_second
           << ", \"grow_seconds\": " << result.grow_seconds
           << ", \"grow_fields_per_second\": " << result.grow_fields_per_second
           << ", \"baseline\": ";
    json_memory(output, result.baseline, 4);
    output << ", \"loaded_before_optimize\": ";
    json_memory(output, result.loaded_before_optimize, 4);
    output << ", \"loaded_after_optimize\": ";
    json_memory(output, result.loaded_after_optimize, 4);
    output << ", \"grown_before_optimize\": ";
    json_memory(output, result.grown_before_optimize, 4);
    output << ", \"grown_after_optimize\": ";
    json_memory(output, result.grown_after_optimize, 4);
    output << ", \"initial_optimize_seconds\": " << result.initial_optimize.seconds
           << ", \"grown_optimize_seconds\": " << result.grown_optimize.seconds << '}';
    if (i + 1 != results.large.size()) output << ',';
    output << '\n';
  }
  output << "  ],\n  \"small_hashes\": [\n";
  for (std::size_t i = 0; i < results.small.size(); ++i) {
    const auto& result = results.small[i];
    output << "    {";
    json_engine(output, result.engine);
    output << ", \"hashes\": " << result.hashes
           << ", \"fields_per_hash\": " << result.fields_per_hash
           << ", \"total_fields\": " << result.total_fields
           << ", \"load_seconds\": " << result.load_seconds
           << ", \"load_hashes_per_second\": " << result.load_hashes_per_second
           << ", \"load_fields_per_second\": " << result.load_fields_per_second
           << ", \"update_hset_operations_per_second\": "
           << result.update_hset_operations_per_second << ", \"baseline\": ";
    json_memory(output, result.baseline, 4);
    output << ", \"loaded\": ";
    json_memory(output, result.loaded, 4);
    output << ", \"after_updates\": ";
    json_memory(output, result.after_updates, 4);
    output << '}';
    if (i + 1 != results.small.size()) output << ',';
    output << '\n';
  }
  output << "  ],\n  \"relocation_density\": [\n";
  for (std::size_t i = 0; i < results.relocation.size(); ++i) {
    const auto& result = results.relocation[i];
    output << "    {";
    json_engine(output, result.engine);
    output << ", \"pattern\": " << json_escape(result.pattern)
           << ", \"density\": " << result.density
           << ", \"fields\": " << result.fields
           << ", \"grown_fields\": " << result.grown_fields
           << ", \"load_fields_per_second\": " << result.load_fields_per_second
           << ", \"first_growth_latency_us\": ";
    json_optional(output, result.first_growth_latency_us);
    output << ", \"hget_operations_per_second\": "
           << result.hget_operations_per_second << ", \"baseline\": ";
    json_memory(output, result.baseline, 4);
    output << ", \"grown_before_optimize\": ";
    json_memory(output, result.grown_before_optimize, 4);
    output << ", \"grown_after_optimize\": ";
    json_memory(output, result.grown_after_optimize, 4);
    output << ", \"optimize_seconds\": " << result.optimize.seconds << '}';
    if (i + 1 != results.relocation.size()) output << ',';
    output << '\n';
  }
  output << "  ],\n  \"mixed_hash_latency\": [\n";
  for (std::size_t i = 0; i < results.mixed.size(); ++i) {
    const auto& result = results.mixed[i];
    output << "    {";
    json_engine(output, result.engine);
    output << ", \"initial_fields\": " << result.initial_fields
           << ", \"samples\": " << result.samples
           << ", \"operations_per_second\": " << result.operations_per_second
           << ", \"latency_p50_us\": " << result.latency_us.p50
           << ", \"latency_p95_us\": " << result.latency_us.p95
           << ", \"latency_p99_us\": " << result.latency_us.p99
           << ", \"latency_p999_us\": " << result.latency_us.p999
           << ", \"latency_max_us\": " << result.latency_us.maximum
           << ", \"final_fields\": " << result.final_fields
           << ", \"baseline\": ";
    json_memory(output, result.baseline, 4);
    output << ", \"loaded\": ";
    json_memory(output, result.loaded, 4);
    output << ", \"after_mixed\": ";
    json_memory(output, result.after_mixed, 4);
    output << '}';
    if (i + 1 != results.mixed.size()) output << ',';
    output << '\n';
  }
  output << "  ]\n}\n";
}

void write_report(const Config& config, const BenchmarkResults& results) {
  if (!config.report.parent_path().empty()) fs::create_directories(config.report.parent_path());
  std::ofstream out(config.report);
  if (!out) fail("cannot open Markdown report output");
  out << "# Goblin Core HSET Speed and Memory Benchmark\n\n"
      << "Generated by the native C++ harness on a dedicated benchmark host at "
      << results.generated_at << ".\n\n";

  if (!results.large.empty()) {
    const auto rss_per_field = [](const LargeHashResult& result) {
      return static_cast<double>(result.loaded_after_optimize.rss_bytes -
                                 result.baseline.rss_bytes) /
             result.fields;
    };
    const LargeHashResult* leanest_goblin = nullptr;
    const LargeHashResult* fastest_goblin_load = nullptr;
    const LargeHashResult* fastest_incumbent_load = nullptr;
    const LargeHashResult* redis_7 = nullptr;
    const LargeHashResult* redis_8 = nullptr;
    for (const auto& result : results.large) {
      if (result.engine.is_goblin()) {
        if (leanest_goblin == nullptr ||
            rss_per_field(result) < rss_per_field(*leanest_goblin)) {
          leanest_goblin = &result;
        }
        if (fastest_goblin_load == nullptr ||
            result.load_fields_per_second > fastest_goblin_load->load_fields_per_second) {
          fastest_goblin_load = &result;
        }
      } else if (fastest_incumbent_load == nullptr ||
                 result.load_fields_per_second >
                     fastest_incumbent_load->load_fields_per_second) {
        fastest_incumbent_load = &result;
      }
      if (result.engine.label == "redis-7.2.4") redis_7 = &result;
      if (result.engine.label == "redis-8.8") redis_8 = &result;
    }

    int goblin_update_rows = 0;
    int goblin_update_wins = 0;
    for (const auto& result : results.small) {
      if (!result.engine.is_goblin()) continue;
      ++goblin_update_rows;
      double incumbent_best = 0.0;
      for (const auto& candidate : results.small) {
        if (candidate.fields_per_hash == result.fields_per_hash &&
            !candidate.engine.is_goblin()) {
          incumbent_best = std::max(incumbent_best,
                                    candidate.update_hset_operations_per_second);
        }
      }
      if (result.update_hset_operations_per_second > incumbent_best) {
        ++goblin_update_wins;
      }
    }

    out << "## Summary\n\n";
    if (leanest_goblin != nullptr) {
      out << "At one million fields, the leaner Goblin transport uses `"
          << fixed(rss_per_field(*leanest_goblin)) << "` RSS bytes/field";
      if (redis_8 != nullptr) {
        out << ", `"
            << fixed((1.0 - rss_per_field(*leanest_goblin) /
                               rss_per_field(*redis_8)) *
                         100.0,
                     1)
            << "%` less than Redis 8.8";
      }
      if (redis_7 != nullptr) {
        out << " and `"
            << fixed((1.0 - rss_per_field(*leanest_goblin) /
                               rss_per_field(*redis_7)) *
                         100.0,
                     1)
            << "%` less than Redis 7.2.4";
      }
      out << ". ";
    }
    if (goblin_update_rows != 0) {
      out << "The Goblin TCP and ring rows beat the fastest incumbent in `"
          << goblin_update_wins << "` of `" << goblin_update_rows
          << "` existing-field small-hash HSET comparisons. ";
    }
    if (fastest_goblin_load != nullptr && fastest_incumbent_load != nullptr) {
      out << "Goblin loses the million-field construction race: its faster `"
          << fastest_goblin_load->engine.label << "` row is `"
          << fixed((1.0 - fastest_goblin_load->load_fields_per_second /
                             fastest_incumbent_load->load_fields_per_second) *
                       100.0,
                   1)
          << "%` behind `" << fastest_incumbent_load->engine.label << "`. ";
    }
    if (!results.mixed.empty()) {
      double goblin_p999_min = std::numeric_limits<double>::infinity();
      double goblin_p999_max = 0.0;
      double incumbent_p999_min = std::numeric_limits<double>::infinity();
      double incumbent_p999_max = 0.0;
      for (const auto& result : results.mixed) {
        auto& minimum = result.engine.is_goblin() ? goblin_p999_min
                                                  : incumbent_p999_min;
        auto& maximum = result.engine.is_goblin() ? goblin_p999_max
                                                  : incumbent_p999_max;
        minimum = std::min(minimum, result.latency_us.p999);
        maximum = std::max(maximum, result.latency_us.p999);
      }
      if (std::isfinite(goblin_p999_min) && std::isfinite(incumbent_p999_min)) {
        out << "The remaining weakness is insertion-compaction tail latency: "
            << "Goblin's mixed p99.9 is `" << fixed(goblin_p999_min)
            << "-" << fixed(goblin_p999_max)
            << " us`, versus `" << fixed(incumbent_p999_min) << "-"
            << fixed(incumbent_p999_max) << " us` for the incumbents.";
      }
    }
    out << "\n\n";
  }

  out << "## Method\n\n"
      << "- Every scenario starts a fresh server; engines run one at a time.\n"
      << "- Linux affinity: server core `" << config.server_core
      << "`, client/load-generator core `" << config.client_core
      << "` (`-1` means unpinned). Ring client and server must not share a core.\n"
      << "- Before the empty-server baseline, every engine constructs and deletes a `"
      << grouped(config.baseline_warmup_fields)
      << "`-field fixed-width hash, then settles.\n"
      << "- RSS is read from the launched server PID: mini-redis-go uses `ps -o rss=`, while other engines use `/proc/<pid>/status` as `VmRSS + HugetlbPages`; no server-reported RSS field is used.\n"
      << "- Transport matrix: Goblin Core is measured over RESP/TCP and typed SBE/shared-memory ring; every incumbent is measured over RESP/TCP. No UDS row is included.\n"
      << "- Bulk workloads, response validation, mixed latency, process control, aggregation, and report generation run in this native C++ executable. TCP point probes use `redis-benchmark`; ring point probes use the typed C++ SBE client.\n"
      << "- Bulk loads use multi-field `HSET` batches of `" << config.hset_batch
      << "`. RESP/TCP uses pipeline depth `" << config.pipeline
      << "`; SBE/ring currently keeps one request outstanding. Timing includes native client encoding. Point rates use `"
      << grouped(config.requests) << "` requests and the median of `" << config.rounds
      << "` probes in each of `" << config.construction_rounds << "` fresh servers.\n"
      << "- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. mini-redis-go runs as an external binary with `GOMAXPROCS=1`, AOF disabled, and metrics disabled.\n"
      << "- Goblin uses default string encoding and its configured compact-hash policy. Incumbents are exercised strictly as black-box RESP servers.\n\n";

  if (!results.large.empty()) {
    out << "## One Large Hash\n\n`" << grouped(config.large_fields)
        << "` distinct `18`-byte fields start with `" << config.value_bytes
        << "`-byte values and then grow to `" << config.grown_value_bytes << "` bytes.\n\n"
        << "### Speed\n\n"
        << "| Engine | bulk new fields/s | same-width HSET ops/s | bulk grow fields/s | initial optimize (s) | grown optimize (s) |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.large) {
      out << "| `" << result.engine.label << "` | "
          << grouped(std::llround(result.load_fields_per_second)) << " | "
          << grouped(std::llround(result.update_hset_operations_per_second)) << " | "
          << grouped(std::llround(result.grow_fields_per_second)) << " | "
          << (result.engine.is_goblin() ? fixed(result.initial_optimize.seconds, 4) : "n/a")
          << " | "
          << (result.engine.is_goblin() ? fixed(result.grown_optimize.seconds, 4) : "n/a")
          << " |\n";
    }
    out << "\n### Memory With " << config.value_bytes << "-Byte Values\n\n"
        << "Goblin is sampled after `GOBLIN.OPTIMIZE`.\n\n"
        << "| Engine | RSS MiB | RSS delta MiB | RSS B/field | used B/field | key B/field |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.large) {
      const auto rss_delta = result.loaded_after_optimize.rss_bytes - result.baseline.rss_bytes;
      const auto used_delta = memory_delta(result.loaded_after_optimize.used_memory_bytes,
                                            result.baseline.used_memory_bytes);
      out << "| `" << result.engine.label << "` | "
          << fixed(result.loaded_after_optimize.rss_bytes / 1048576.0) << " | "
          << fixed(rss_delta / 1048576.0) << " | "
          << fixed(static_cast<double>(rss_delta) / result.fields) << " | "
          << optional_fixed(memory_per(used_delta, result.fields)) << " | "
          << optional_fixed(result.loaded_after_optimize.key_memory_bytes
                                ? std::optional<double>(*result.loaded_after_optimize.key_memory_bytes /
                                                        result.fields)
                                : std::nullopt)
          << " |\n";
    }
    out << "\n### Memory After Growing Every Value to " << config.grown_value_bytes
        << " Bytes\n\n"
        << "| Engine | pre-opt RSS B/field | post-opt RSS B/field | post-opt used B/field | post-opt key B/field |\n"
        << "| --- | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.large) {
      const auto pre_rss = result.grown_before_optimize.rss_bytes - result.baseline.rss_bytes;
      const auto post_rss = result.grown_after_optimize.rss_bytes - result.baseline.rss_bytes;
      const auto post_used = memory_delta(result.grown_after_optimize.used_memory_bytes,
                                          result.baseline.used_memory_bytes);
      out << "| `" << result.engine.label << "` | "
          << fixed(static_cast<double>(pre_rss) / result.fields) << " | "
          << fixed(static_cast<double>(post_rss) / result.fields) << " | "
          << optional_fixed(memory_per(post_used, result.fields)) << " | "
          << optional_fixed(result.grown_after_optimize.key_memory_bytes
                                ? std::optional<double>(*result.grown_after_optimize.key_memory_bytes /
                                                        result.fields)
                                : std::nullopt)
          << " |\n";
    }
    const bool has_goblin = std::any_of(
        results.large.begin(), results.large.end(),
        [](const auto& result) { return result.engine.is_goblin(); });
    if (has_goblin) {
      out << "\n### Goblin Large-Hash Internals\n\n"
          << "| Engine | Phase | fields | live MiB | dead MiB | arena MiB | index MiB | total MiB |\n"
          << "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n";
      const auto internal_row = [&](const LargeHashResult& goblin,
                                    std::string_view phase,
                                    const MemorySample& sample) {
        out << "| `" << goblin.engine.label << "` | `" << phase << "` | "
            << grouped(goblin.fields) << " | "
            << fixed(goblin_stat(sample, "field_value_live_bytes") / 1048576.0) << " | "
            << fixed(goblin_stat(sample, "field_value_dead_bytes") / 1048576.0) << " | "
            << fixed(goblin_stat(sample, "field_value_allocated_bytes") / 1048576.0) << " | "
            << fixed(goblin_stat(sample, "field_index_allocated_bytes") / 1048576.0) << " | "
            << fixed(goblin_stat(sample, "total_allocated_bytes") / 1048576.0) << " |\n";
      };
      for (const auto& goblin : results.large) {
        if (!goblin.engine.is_goblin()) continue;
        internal_row(goblin, "loaded before optimize", goblin.loaded_before_optimize);
        internal_row(goblin, "loaded after optimize", goblin.loaded_after_optimize);
        internal_row(goblin, "grown before optimize", goblin.grown_before_optimize);
        internal_row(goblin, "grown after optimize", goblin.grown_after_optimize);
      }
    }
  }

  if (!results.small.empty()) {
    out << "\n## Many Hashes\n\nEach shape holds approximately `"
        << grouped(config.small_total_fields)
        << "` total fields. Keys are `14` bytes, fields are `5` bytes, and values are `"
        << config.value_bytes << "` bytes.\n\n### Speed\n\n"
        << "| fields/hash | hashes | Engine | load fields/s | load hashes/s | middle-field HSET ops/s |\n"
        << "| ---: | ---: | --- | ---: | ---: | ---: |\n";
    for (const auto& result : results.small) {
      out << "| " << result.fields_per_hash << " | " << grouped(result.hashes)
          << " | `" << result.engine.label << "` | "
          << grouped(std::llround(result.load_fields_per_second)) << " | "
          << grouped(std::llround(result.load_hashes_per_second)) << " | "
          << grouped(std::llround(result.update_hset_operations_per_second)) << " |\n";
    }
    out << "\n### Memory\n\n"
        << "Sampled key bytes are the mean of the first, middle, and last hash.\n\n"
        << "| fields/hash | Engine | RSS MiB | RSS B/hash | RSS B/field | used B/hash | sampled key B/hash |\n"
        << "| ---: | --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.small) {
      const auto rss_delta = result.loaded.rss_bytes - result.baseline.rss_bytes;
      const auto used_delta = memory_delta(result.loaded.used_memory_bytes,
                                            result.baseline.used_memory_bytes);
      out << "| " << result.fields_per_hash << " | `" << result.engine.label << "` | "
          << fixed(result.loaded.rss_bytes / 1048576.0) << " | "
          << fixed(static_cast<double>(rss_delta) / result.hashes) << " | "
          << fixed(static_cast<double>(rss_delta) / result.total_fields) << " | "
          << optional_fixed(memory_per(used_delta, result.hashes)) << " | "
          << optional_fixed(result.loaded.key_memory_bytes) << " |\n";
    }
  }

  if (!results.relocation.empty()) {
    out << "\n## Goblin Full-Hash Relocation Density\n\n"
        << "Goblin uses `--hash-listpack-max-entries 0`. Each row is the median of `"
        << config.relocation_rounds << "` fresh-server rounds over `"
        << grouped(config.relocation_fields) << "` fields.\n\n"
        << "| pattern | density | grown fields | first grow us | HGET ops/s | pre-opt RSS B/field | post-opt RSS B/field | dead MiB | compact ms |\n"
        << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.relocation) {
      const auto pre = result.grown_before_optimize.rss_bytes - result.baseline.rss_bytes;
      const auto post = result.grown_after_optimize.rss_bytes - result.baseline.rss_bytes;
      out << "| `" << result.pattern << "` | " << fixed(result.density * 100.0, 1)
          << "% | " << grouped(result.grown_fields) << " | "
          << optional_fixed(result.first_growth_latency_us) << " | "
          << grouped(std::llround(result.hget_operations_per_second)) << " | "
          << fixed(static_cast<double>(pre) / result.fields) << " | "
          << fixed(static_cast<double>(post) / result.fields) << " | "
          << fixed(goblin_stat(result.grown_before_optimize, "field_value_dead_bytes") /
                   1048576.0)
          << " | " << fixed(result.optimize.seconds * 1000.0) << " |\n";
    }
  }

  if (!results.mixed.empty()) {
    out << "\n## Mixed Hash Write Latency\n\n"
        << "A seeded depth-one workload starts with `" << grouped(config.mixed_fields)
        << "` fields, warms `" << grouped(config.mixed_warmup) << "` operations, and measures `"
        << grouped(config.mixed_samples) << "` unpipelined transport round trips in C++.\n\n"
        << "| Engine | ops/s | insert/update/delete/grow | p50 us | p95 us | p99 us | p99.9 us | max us | RSS B/field | dead MiB | compacting | final fields |\n"
        << "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& result : results.mixed) {
      const auto rss_delta = result.after_mixed.rss_bytes - result.baseline.rss_bytes;
      out << "| `" << result.engine.label << "` | "
          << grouped(std::llround(result.operations_per_second)) << " | "
          << result.operations[0] << '/' << result.operations[1] << '/'
          << result.operations[2] << '/' << result.operations[3] << " | "
          << fixed(result.latency_us.p50) << " | " << fixed(result.latency_us.p95)
          << " | " << fixed(result.latency_us.p99) << " | "
          << fixed(result.latency_us.p999) << " | " << fixed(result.latency_us.maximum)
          << " | " << fixed(static_cast<double>(rss_delta) / result.final_fields) << " | ";
      if (result.engine.is_goblin()) {
        out << fixed(goblin_stat(result.after_mixed, "field_value_dead_bytes") / 1048576.0)
            << " | " << goblin_stat(result.after_mixed, "field_compaction_active");
      } else {
        out << "n/a | n/a";
      }
      out << " | " << grouped(result.final_fields) << " |\n";
    }
    out << "\n| Engine | operation | p50 us | p95 us | p99 us | p99.9 us | max us |\n"
        << "| --- | --- | ---: | ---: | ---: | ---: | ---: |\n";
    constexpr std::array<std::string_view, 4> names{"insert", "update", "delete", "grow"};
    for (const auto& result : results.mixed) {
      for (std::size_t operation = 0; operation < names.size(); ++operation) {
        const auto& latency = result.operation_latency_us[operation];
        out << "| `" << result.engine.label << "` | " << names[operation] << " | "
            << fixed(latency.p50) << " | " << fixed(latency.p95) << " | "
            << fixed(latency.p99) << " | " << fixed(latency.p999) << " | "
            << fixed(latency.maximum) << " |\n";
      }
    }
  }

  out << "\nThe medium-hash representation crossover is measured separately in "
      << "[HASH-THRESHOLD-SWEEP.md](HASH-THRESHOLD-SWEEP.md).\n\n"
      << "## Tested Servers\n\n";
  for (const auto& engine : config.engines) out << "- " << engine.label << '\n';
}

BenchmarkResults run_benchmarks(const Config& config) {
  BenchmarkResults results;
  results.generated_at = utc_timestamp();
  if (!config.skip_large) {
    for (const auto& engine : config.engines) {
      std::cerr << "benchmarking large hash on " << engine.label << "...\n";
      auto result = run_large(engine, config);
      const auto rss = result.loaded_after_optimize.rss_bytes - result.baseline.rss_bytes;
      std::cerr << "  load " << grouped(std::llround(result.load_fields_per_second))
                << " fields/s, update "
                << grouped(std::llround(result.update_hset_operations_per_second))
                << " ops/s, RSS " << fixed(static_cast<double>(rss) / result.fields, 1)
                << " B/field\n";
      results.large.push_back(std::move(result));
    }
  }
  if (!config.skip_small) {
    for (const int shape : config.small_shapes) {
      for (const auto& engine : config.engines) {
        std::cerr << "benchmarking " << shape << " fields/hash on " << engine.label
                  << "...\n";
        auto result = run_small(engine, config, shape);
        const auto rss = result.loaded.rss_bytes - result.baseline.rss_bytes;
        std::cerr << "  load " << grouped(std::llround(result.load_fields_per_second))
                  << " fields/s, update "
                  << grouped(std::llround(result.update_hset_operations_per_second))
                  << " ops/s, RSS " << fixed(static_cast<double>(rss) / result.hashes, 1)
                  << " B/hash\n";
        results.small.push_back(std::move(result));
      }
    }
  }
  if (!config.skip_relocation) {
    for (const auto& engine : config.engines) {
      if (!engine.is_goblin()) continue;
      for (const auto pattern : {std::string_view("random"), std::string_view("clustered")}) {
        for (const double density : config.relocation_densities) {
          std::cerr << "benchmarking " << pattern << " relocation density "
                    << density * 100.0 << "% on " << engine.label << "...\n";
          auto result = run_relocation(engine, config, pattern, density);
          std::cerr << "  first grow " << optional_fixed(result.first_growth_latency_us)
                    << " us, HGET "
                    << grouped(std::llround(result.hget_operations_per_second))
                    << " ops/s, compact " << fixed(result.optimize.seconds * 1000.0)
                    << " ms\n";
          results.relocation.push_back(std::move(result));
        }
      }
    }
  }
  if (!config.skip_mixed) {
    for (const auto& engine : config.engines) {
      std::cerr << "benchmarking mixed hash latency on " << engine.label << "...\n";
      auto result = run_mixed(engine, config);
      std::cerr << "  " << grouped(std::llround(result.operations_per_second))
                << " ops/s, p99 " << fixed(result.latency_us.p99)
                << " us, p99.9 " << fixed(result.latency_us.p999) << " us\n";
      results.mixed.push_back(std::move(result));
    }
  }
  write_json(config, results);
  write_report(config, results);
  return results;
}

}  // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  try {
    const Config config = parse_args(argc, argv);
    pin_current_process(config.client_core);
    (void)run_benchmarks(config);
    std::cout << "wrote " << config.output_json << '\n'
              << "wrote " << config.report << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hset_benchmark: " << error.what() << '\n';
    return 1;
  }
}
