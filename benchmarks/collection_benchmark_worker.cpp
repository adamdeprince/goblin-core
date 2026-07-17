// Native timed client for the SET and ARRAY benchmark orchestrator.
//
// The Python runner owns process lifecycle, RSS sampling, and report rendering.
// Every timed command is generated, sent, and decoded here so Python scheduling
// does not become part of the measured path.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using goblin::core::SbeRingClient;

struct Config {
  std::string suite;
  std::string action;
  std::string transport = "resp";
  std::string host = "127.0.0.1";
  std::string ring;
  std::string key = "bench:primary";
  std::string second_key = "bench:secondary";
  std::string array_mode = "classic";
  int port = 6379;
  std::int64_t members = 1'000'000;
  std::int64_t requests = 200'000;
  std::int64_t offset = 0;
  std::int64_t stride = 1;
  int value_bytes = 16;
  int batch = 128;
  int pipeline = 256;
  std::size_t read_buffer_bytes = 64U * 1024U;
  std::size_t array_reserve_bytes = 0;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

std::string next_arg(int argc, char** argv, int& index) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + argv[index]);
  return argv[++index];
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--suite") config.suite = next_arg(argc, argv, i);
    else if (arg == "--action") config.action = next_arg(argc, argv, i);
    else if (arg == "--transport") config.transport = next_arg(argc, argv, i);
    else if (arg == "--host") config.host = next_arg(argc, argv, i);
    else if (arg == "--port") config.port = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--ring") config.ring = next_arg(argc, argv, i);
    else if (arg == "--key") config.key = next_arg(argc, argv, i);
    else if (arg == "--second-key") config.second_key = next_arg(argc, argv, i);
    else if (arg == "--array-mode") config.array_mode = next_arg(argc, argv, i);
    else if (arg == "--members") config.members = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--requests") config.requests = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--offset") config.offset = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--stride") config.stride = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--value-bytes") config.value_bytes = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--batch") config.batch = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--pipeline") config.pipeline = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--read-buffer-bytes") {
      config.read_buffer_bytes = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--array-reserve-bytes") {
      config.array_reserve_bytes = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: " << argv[0] << " --suite set|array|zset --action ACTION [options]\n"
          << "  --transport resp|sbe --host HOST --port PORT --ring PATH\n"
          << "  --array-mode classic|rt|native --members N --requests N\n"
          << "  --batch N --pipeline N --value-bytes N --stride N\n"
          << "  --array-reserve-bytes N (RT increment latency only)\n";
      std::exit(0);
    } else {
      fail("unknown argument: " + arg);
    }
  }
  if (config.suite != "set" && config.suite != "array" &&
      config.suite != "zset") {
    fail("--suite must be set, array, or zset");
  }
  if (config.action.empty()) fail("--action is required");
  if (config.transport != "resp" && config.transport != "sbe") {
    fail("--transport must be resp or sbe");
  }
  if (config.transport == "sbe" && config.ring.empty()) {
    fail("--ring is required for SBE");
  }
  if (config.suite == "array" && config.transport != "resp") {
    fail("ARRAY benchmark supports RESP only");
  }
  if (config.array_mode != "classic" && config.array_mode != "rt" &&
      config.array_mode != "native") {
    fail("--array-mode must be classic, rt, or native");
  }
  if (config.members <= 0 || config.requests <= 0 || config.stride <= 0 ||
      config.value_bytes < 2 || config.batch <= 0 || config.pipeline <= 0) {
    fail("benchmark counts are out of range");
  }
  return config;
}

std::string fixed_token(std::int64_t value, int width, char marker) {
  if (width < 2) fail("token width is too small");
  std::array<char, 32> digits{};
  const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                       value, 16);
  if (converted.ec != std::errc{}) fail("token formatting failed");
  const auto count = static_cast<std::size_t>(converted.ptr - digits.data());
  const auto available = static_cast<std::size_t>(width - 1);
  if (count > available) fail("token value exceeds configured width");
  std::string result(1, marker);
  result.append(available - count, '0');
  result.append(digits.data(), converted.ptr);
  return result;
}

struct RespValue {
  enum class Type { nil, string, integer, array } type = Type::nil;
  std::string string;
  std::int64_t integer = 0;
  std::vector<RespValue> array;
};

class RespClient {
 public:
  RespClient(std::string_view host, int port, std::size_t read_buffer_bytes)
      : input_(std::max<std::size_t>(read_buffer_bytes, 4096)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) fail(std::string("socket: ") + std::strerror(errno));
    int one = 1;
    (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval timeout{30, 0};
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, std::string(host).c_str(), &address.sin_addr) != 1) {
      fail("invalid IPv4 benchmark host");
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      const auto message = std::string("connect: ") + std::strerror(errno);
      ::close(fd_);
      fd_ = -1;
      fail(message);
    }
  }

  RespClient(const RespClient&) = delete;
  RespClient& operator=(const RespClient&) = delete;
  ~RespClient() {
    if (fd_ >= 0) ::close(fd_);
  }

  static void append_command(std::string& output,
                             const std::vector<std::string>& command) {
    output.push_back('*');
    output += std::to_string(command.size());
    output += "\r\n";
    for (const auto& argument : command) {
      output.push_back('$');
      output += std::to_string(argument.size());
      output += "\r\n";
      output += argument;
      output += "\r\n";
    }
  }

  template <class MakeCommand, class ReadReply>
  std::uint64_t pipeline(std::int64_t count, int depth, MakeCommand&& make_command,
                         ReadReply&& read_reply) {
    std::uint64_t checksum = 0;
    for (std::int64_t begin = 0; begin < count; begin += depth) {
      const auto commands = std::min<std::int64_t>(depth, count - begin);
      std::string output;
      output.reserve(static_cast<std::size_t>(commands) * 96U);
      for (std::int64_t offset = 0; offset < commands; ++offset) {
        append_command(output, make_command(begin + offset));
      }
      send_all(output);
      for (std::int64_t offset = 0; offset < commands; ++offset) {
        checksum += read_reply(read_response());
      }
    }
    return checksum;
  }

  RespValue command(const std::vector<std::string>& command) {
    std::string output;
    output.reserve(96U);
    append_command(output, command);
    send_all(output);
    return read_response();
  }

 private:
  int fd_ = -1;
  std::vector<char> input_;
  std::size_t input_pos_ = 0;
  std::size_t input_size_ = 0;

  void send_all(std::string_view data) {
    while (!data.empty()) {
      const auto sent = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
      if (sent <= 0) fail(std::string("send: ") + std::strerror(errno));
      data.remove_prefix(static_cast<std::size_t>(sent));
    }
  }

  void receive_more() {
    if (input_pos_ != 0 && input_pos_ != input_size_) {
      std::memmove(input_.data(), input_.data() + input_pos_, input_size_ - input_pos_);
      input_size_ -= input_pos_;
      input_pos_ = 0;
    } else if (input_pos_ == input_size_) {
      input_pos_ = 0;
      input_size_ = 0;
    }
    if (input_size_ == input_.size()) input_.resize(input_.size() * 2U);
    const auto count = ::recv(fd_, input_.data() + input_size_,
                              input_.size() - input_size_, 0);
    if (count <= 0) fail(std::string("recv: ") + std::strerror(errno));
    input_size_ += static_cast<std::size_t>(count);
  }

  char read_char() {
    if (input_pos_ == input_size_) receive_more();
    return input_[input_pos_++];
  }

  std::string read_line() {
    std::string result;
    while (true) {
      const char value = read_char();
      if (value == '\r') {
        if (read_char() != '\n') fail("invalid RESP line ending");
        return result;
      }
      result.push_back(value);
    }
  }

  std::string read_bytes(std::size_t count) {
    std::string result;
    result.reserve(count);
    while (count != 0) {
      if (input_pos_ == input_size_) receive_more();
      const auto available = std::min(count, input_size_ - input_pos_);
      result.append(input_.data() + input_pos_, available);
      input_pos_ += available;
      count -= available;
    }
    return result;
  }

  static std::int64_t parse_integer(std::string_view text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      fail("invalid RESP integer");
    }
    return value;
  }

  RespValue read_response() {
    const char prefix = read_char();
    if (prefix == '+' || prefix == '-') {
      auto line = read_line();
      if (prefix == '-') fail("server returned error: " + line);
      RespValue result;
      result.type = RespValue::Type::string;
      result.string = std::move(line);
      return result;
    }
    if (prefix == ':') {
      RespValue result;
      result.type = RespValue::Type::integer;
      result.integer = parse_integer(read_line());
      return result;
    }
    if (prefix == '$') {
      const auto length = parse_integer(read_line());
      if (length < 0) return {};
      RespValue result;
      result.type = RespValue::Type::string;
      result.string = read_bytes(static_cast<std::size_t>(length));
      if (read_bytes(2) != "\r\n") fail("invalid RESP bulk terminator");
      return result;
    }
    if (prefix == '*') {
      const auto count = parse_integer(read_line());
      if (count < 0) return {};
      RespValue result;
      result.type = RespValue::Type::array;
      result.array.reserve(static_cast<std::size_t>(count));
      for (std::int64_t i = 0; i < count; ++i) {
        result.array.push_back(read_response());
      }
      return result;
    }
    fail("unsupported RESP reply prefix");
  }
};

std::uint64_t reply_checksum(const RespValue& reply) {
  switch (reply.type) {
    case RespValue::Type::nil:
      return 1;
    case RespValue::Type::integer:
      return static_cast<std::uint64_t>(reply.integer + 17);
    case RespValue::Type::string:
      return reply.string.size() + 31U;
    case RespValue::Type::array: {
      std::uint64_t result = reply.array.size() + 47U;
      for (const auto& value : reply.array) result += reply_checksum(value);
      return result;
    }
  }
  return 0;
}

std::string array_command(const Config& config, std::string_view operation) {
  if (config.array_mode == "native") {
    return "AR" + std::string(operation);
  }
  const std::string prefix = config.array_mode == "rt" ? "GOBLIN.RT.AR" :
                                                          "GOBLIN.CLASSIC.AR";
  return prefix + std::string(operation);
}

std::vector<std::string> set_command(const Config& config, std::int64_t ordinal) {
  if (config.action == "load") {
    const auto begin = ordinal * config.batch;
    const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
    std::vector<std::string> command{"SADD", config.key};
    command.reserve(static_cast<std::size_t>(count) + 2U);
    for (std::int64_t offset = 0; offset < count; ++offset) {
      command.push_back(fixed_token(config.offset + begin + offset,
                                    config.value_bytes, 'm'));
    }
    return command;
  }
  const auto member = ordinal % config.members;
  if (config.action == "member-hit") {
    return {"SISMEMBER", config.key,
            fixed_token(config.offset + member, config.value_bytes, 'm')};
  }
  if (config.action == "member-miss") {
    return {"SISMEMBER", config.key,
            fixed_token(config.offset + config.members + member,
                        config.value_bytes, 'x')};
  }
  if (config.action == "multi-member") {
    std::vector<std::string> command{"SMISMEMBER", config.key};
    for (int i = 0; i < 4; ++i) {
      command.push_back(fixed_token(config.offset + (member + i) % config.members,
                                    config.value_bytes, 'm'));
    }
    return command;
  }
  if (config.action == "card") return {"SCARD", config.key};
  if (config.action == "intercard") {
    return {"SINTERCARD", "2", config.key, config.second_key};
  }
  if (config.action == "churn") {
    const auto pair = ordinal / 2;
    const auto value = fixed_token(config.offset + pair % config.members,
                                   config.value_bytes, 'm');
    return {ordinal % 2 == 0 ? "SREM" : "SADD", config.key, value};
  }
  fail("unsupported SET action: " + config.action);
}

std::vector<std::string> array_command_at(const Config& config,
                                          std::int64_t ordinal) {
  if (config.action == "load") {
    const auto begin = ordinal * config.batch;
    const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
    const bool dense_native = config.stride == 1;
    std::vector<std::string> command{
        array_command(config, dense_native ? "SET" : "MSET"), config.key};
    if (dense_native) command.push_back(std::to_string(config.offset + begin));
    command.reserve(static_cast<std::size_t>(count * (dense_native ? 1 : 2)) + 3U);
    for (std::int64_t offset = 0; offset < count; ++offset) {
      const auto item = begin + offset;
      if (!dense_native) {
        command.push_back(std::to_string((config.offset + item) * config.stride));
      }
      command.push_back(fixed_token(config.offset + item, config.value_bytes, 'v'));
    }
    return command;
  }
  if (config.action == "increment-latency") {
    const auto index = config.offset + ordinal;
    return {array_command(config, "SET"), config.key, std::to_string(index),
            fixed_token(index, config.value_bytes, 'v')};
  }
  const auto item = ordinal % config.members;
  const auto index = (config.offset + item) * config.stride;
  if (config.action == "get-hit") {
    return {array_command(config, "GET"), config.key, std::to_string(index)};
  }
  if (config.action == "get-miss") {
    const auto missing = (config.offset + config.members + item) * config.stride + 1;
    return {array_command(config, "GET"), config.key, std::to_string(missing)};
  }
  if (config.action == "multi-get") {
    std::vector<std::string> command{array_command(config, "MGET"), config.key};
    for (int i = 0; i < 4; ++i) {
      const auto other = (config.offset + (item + i) % config.members) * config.stride;
      command.push_back(std::to_string(other));
    }
    return command;
  }
  if (config.action == "count") {
    return {array_command(config, "COUNT"), config.key};
  }
  if (config.action == "length") {
    return {array_command(config, "LEN"), config.key};
  }
  if (config.action == "insert") {
    return {array_command(config, "INSERT"), config.key,
            fixed_token(config.offset + config.members + ordinal,
                        config.value_bytes, 'i')};
  }
  if (config.action == "update") {
    return {array_command(config, "SET"), config.key, std::to_string(index),
            fixed_token(config.offset + item, config.value_bytes, 'u')};
  }
  if (config.action == "churn") {
    const auto pair = ordinal / 2;
    const auto churn_item = pair % config.members;
    const auto churn_index = (config.offset + churn_item) * config.stride;
    if (ordinal % 2 == 0) {
      return {array_command(config, "DEL"), config.key,
              std::to_string(churn_index)};
    }
    return {array_command(config, "MSET"), config.key,
            std::to_string(churn_index),
            fixed_token(config.offset + churn_item, config.value_bytes, 'v')};
  }
  fail("unsupported ARRAY action: " + config.action);
}

std::uint32_t zset_score(std::int64_t item) {
  return static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(item) * 1'103'515'245ULL + 12'345ULL);
}

std::vector<std::string> zset_command(const Config& config,
                                      std::int64_t ordinal) {
  if (config.action == "load") {
    const auto begin = ordinal * config.batch;
    const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
    std::vector<std::string> command{"ZADD", config.key};
    command.reserve(static_cast<std::size_t>(count * 2) + 2U);
    for (std::int64_t offset = 0; offset < count; ++offset) {
      const auto item = config.offset + begin + offset;
      command.push_back(std::to_string(zset_score(item)));
      command.push_back(fixed_token(item, config.value_bytes, 'z'));
    }
    return command;
  }
  const auto item = config.offset + ordinal % config.members;
  const auto member = fixed_token(item, config.value_bytes, 'z');
  if (config.action == "score-hit") return {"ZSCORE", config.key, member};
  if (config.action == "rank") return {"ZRANK", config.key, member};
  if (config.action == "range-16") return {"ZRANGE", config.key, "0", "15"};
  if (config.action == "card") return {"ZCARD", config.key};
  if (config.action == "update") {
    return {"ZADD", config.key, std::to_string(zset_score(item) + 0.5), member};
  }
  if (config.action == "churn") {
    const auto pair_item = config.offset + (ordinal / 2) % config.members;
    const auto pair_member = fixed_token(pair_item, config.value_bytes, 'z');
    if (ordinal % 2 == 0) return {"ZREM", config.key, pair_member};
    return {"ZADD", config.key, std::to_string(zset_score(pair_item)), pair_member};
  }
  fail("unsupported ZSET action: " + config.action);
}

std::int64_t command_count(const Config& config) {
  if (config.action == "load") {
    return (config.members + config.batch - 1) / config.batch;
  }
  if (config.action == "churn") return config.requests * 2;
  return config.requests;
}

struct TimedResult {
  std::uint64_t checksum = 0;
  double seconds = 0.0;
};

struct LatencyResult {
  TimedResult timed;
  double p90_us = 0.0;
  double p99_us = 0.0;
  double p999_us = 0.0;
  double p9999_us = 0.0;
};

std::uint64_t percentile_ns(const std::vector<std::uint64_t>& sorted_values,
                            std::uint64_t numerator,
                            std::uint64_t denominator) {
  const auto rank =
      (sorted_values.size() * numerator + denominator - 1U) / denominator;
  return sorted_values[std::min(sorted_values.size() - 1U,
                                static_cast<std::size_t>(rank - 1U))];
}

LatencyResult run_array_increment_latency(const Config& config) {
  if (config.pipeline != 1) {
    fail("ARRAY increment latency requires --pipeline 1");
  }
  RespClient client(config.host, config.port, config.read_buffer_bytes);
  const auto count = command_count(config);
  if (config.array_reserve_bytes != 0) {
    if (config.array_mode != "rt") {
      fail("ARRAY reservation requires --array-mode rt");
    }
    const auto max_index = config.offset + count - 1;
    const auto reply = client.command(
        {"GOBLIN.RT.ARRESERVE", config.key, std::to_string(max_index),
         std::to_string(count), std::to_string(config.array_reserve_bytes)});
    if (reply.type != RespValue::Type::integer || reply.integer != 1) {
      fail("GOBLIN.RT.ARRESERVE returned an unexpected reply");
    }
  }
  std::vector<std::uint64_t> latencies_ns;
  latencies_ns.reserve(static_cast<std::size_t>(count));
  std::uint64_t checksum = 0;
  const auto run_started = Clock::now();
  for (std::int64_t ordinal = 0; ordinal < count; ++ordinal) {
    const auto command = array_command_at(config, ordinal);
    const auto started = Clock::now();
    const auto reply = client.command(command);
    const auto finished = Clock::now();
    checksum += reply_checksum(reply);
    latencies_ns.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
            .count()));
  }
  const auto seconds =
      std::chrono::duration<double>(Clock::now() - run_started).count();
  std::sort(latencies_ns.begin(), latencies_ns.end());
  const auto microseconds = [&](std::uint64_t numerator,
                                std::uint64_t denominator) {
    return static_cast<double>(
               percentile_ns(latencies_ns, numerator, denominator)) /
           1'000.0;
  };
  return {{checksum, seconds}, microseconds(90, 100), microseconds(99, 100),
          microseconds(999, 1'000), microseconds(9'999, 10'000)};
}

TimedResult run_resp(const Config& config) {
  RespClient client(config.host, config.port, config.read_buffer_bytes);
  const auto count = command_count(config);
  const auto started = Clock::now();
  const auto checksum = client.pipeline(
      count, config.pipeline,
      [&](std::int64_t ordinal) {
        if (config.suite == "set") return set_command(config, ordinal);
        if (config.suite == "array") return array_command_at(config, ordinal);
        return zset_command(config, ordinal);
      },
      [](const RespValue& reply) { return reply_checksum(reply); });
  return {checksum, std::chrono::duration<double>(Clock::now() - started).count()};
}

std::vector<std::string> set_values(const Config& config, std::int64_t begin,
                                    std::int64_t count) {
  std::vector<std::string> values;
  values.reserve(static_cast<std::size_t>(count));
  for (std::int64_t offset = 0; offset < count; ++offset) {
    values.push_back(fixed_token(config.offset + begin + offset,
                                 config.value_bytes, 'm'));
  }
  return values;
}

void enqueue_set_command(SbeRingClient& client, const Config& config,
                         std::int64_t ordinal) {
  if (config.action == "load") {
    const auto begin = ordinal * config.batch;
    const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
    const auto values = set_values(config, begin, count);
    std::size_t need = config.key.size();
    for (const auto& value : values) need += value.size();
    client.enqueue_sbe<goblin_sbe::SAdd>(need, [&](auto& message) {
      auto& entries = message.membersCount(static_cast<std::uint16_t>(values.size()));
      for (const auto& value : values) {
        entries.next().putMember(value.data(), static_cast<std::uint32_t>(value.size()));
      }
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  const auto member = ordinal % config.members;
  if (config.action == "member-hit" || config.action == "member-miss") {
    const bool hit = config.action == "member-hit";
    const auto value = fixed_token(config.offset + (hit ? member : config.members + member),
                                   config.value_bytes, hit ? 'm' : 'x');
    client.enqueue_sbe<goblin_sbe::SIsMember>(config.key.size() + value.size(),
                                              [&](auto& message) {
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      message.putMember(value.data(), static_cast<std::uint32_t>(value.size()));
    });
    return;
  }
  if (config.action == "multi-member") {
    std::vector<std::string> values;
    values.reserve(4);
    std::size_t need = config.key.size();
    for (int i = 0; i < 4; ++i) {
      values.push_back(fixed_token(config.offset + (member + i) % config.members,
                                   config.value_bytes, 'm'));
      need += values.back().size();
    }
    client.enqueue_sbe<goblin_sbe::SMIsMember>(need, [&](auto& message) {
      auto& entries = message.membersCount(static_cast<std::uint16_t>(values.size()));
      for (const auto& value : values) {
        entries.next().putMember(value.data(), static_cast<std::uint32_t>(value.size()));
      }
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  if (config.action == "card") {
    client.enqueue_sbe<goblin_sbe::SCard>(config.key.size(), [&](auto& message) {
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  if (config.action == "intercard") {
    const std::array<std::string_view, 2> keys{config.key, config.second_key};
    client.enqueue_sbe<goblin_sbe::SInterCard>(config.key.size() + config.second_key.size(),
                                               [&](auto& message) {
      message.limit(0);
      auto& entries = message.keysCount(static_cast<std::uint16_t>(keys.size()));
      for (const auto key : keys) {
        entries.next().putKey(key.data(), static_cast<std::uint32_t>(key.size()));
      }
    });
    return;
  }
  if (config.action == "churn") {
    const auto pair = ordinal / 2;
    const auto value = fixed_token(config.offset + pair % config.members,
                                   config.value_bytes, 'm');
    if (ordinal % 2 == 0) {
      client.enqueue_sbe<goblin_sbe::SRem>(config.key.size() + value.size(),
                                           [&](auto& message) {
        auto& entries = message.membersCount(1);
        entries.next().putMember(value.data(), static_cast<std::uint32_t>(value.size()));
        message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      });
    } else {
      client.enqueue_sbe<goblin_sbe::SAdd>(config.key.size() + value.size(),
                                           [&](auto& message) {
        auto& entries = message.membersCount(1);
        entries.next().putMember(value.data(), static_cast<std::uint32_t>(value.size()));
        message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      });
    }
    return;
  }
  fail("unsupported SBE SET action: " + config.action);
}

void enqueue_zset_command(SbeRingClient& client, const Config& config,
                          std::int64_t ordinal) {
  if (config.action == "load") {
    const auto begin = ordinal * config.batch;
    const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
    std::vector<std::string> members;
    members.reserve(static_cast<std::size_t>(count));
    for (std::int64_t offset = 0; offset < count; ++offset) {
      members.push_back(fixed_token(config.offset + begin + offset,
                                    config.value_bytes, 'z'));
    }
    std::size_t need = config.key.size();
    for (const auto& member : members) need += member.size();
    client.enqueue_sbe<goblin_sbe::ZAdd>(need, [&](auto& message) {
      message.flags(0);
      auto& entries = message.membersCount(static_cast<std::uint16_t>(members.size()));
      for (std::size_t i = 0; i < members.size(); ++i) {
        const auto item = config.offset + begin + static_cast<std::int64_t>(i);
        entries.next()
            .score(static_cast<double>(zset_score(item)))
            .putMember(members[i].data(), static_cast<std::uint32_t>(members[i].size()));
      }
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  const auto item = config.offset + ordinal % config.members;
  const auto member = fixed_token(item, config.value_bytes, 'z');
  if (config.action == "score-hit") {
    client.enqueue_sbe<goblin_sbe::ZScore>(config.key.size() + member.size(),
                                           [&](auto& message) {
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      message.putMember(member.data(), static_cast<std::uint32_t>(member.size()));
    });
    return;
  }
  if (config.action == "rank") {
    client.enqueue_sbe<goblin_sbe::ZRank>(config.key.size() + member.size(),
                                          [&](auto& message) {
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      message.putMember(member.data(), static_cast<std::uint32_t>(member.size()));
    });
    return;
  }
  if (config.action == "range-16") {
    client.enqueue_sbe<goblin_sbe::ZRange>(config.key.size(), [&](auto& message) {
      message.start(0).stop(15).withScores(0).rev(0);
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  if (config.action == "card") {
    client.enqueue_sbe<goblin_sbe::ZCard>(config.key.size(), [&](auto& message) {
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  if (config.action == "update") {
    client.enqueue_sbe<goblin_sbe::ZAdd>(config.key.size() + member.size(),
                                         [&](auto& message) {
      message.flags(0);
      auto& entries = message.membersCount(1);
      entries.next()
          .score(static_cast<double>(zset_score(item)) + 0.5)
          .putMember(member.data(), static_cast<std::uint32_t>(member.size()));
      message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
    });
    return;
  }
  if (config.action == "churn") {
    const auto pair_item = config.offset + (ordinal / 2) % config.members;
    const auto pair_member = fixed_token(pair_item, config.value_bytes, 'z');
    if (ordinal % 2 == 0) {
      client.enqueue_sbe<goblin_sbe::ZRem>(config.key.size() + pair_member.size(),
                                           [&](auto& message) {
        auto& entries = message.membersCount(1);
        entries.next().putMember(pair_member.data(),
                                 static_cast<std::uint32_t>(pair_member.size()));
        message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      });
    } else {
      client.enqueue_sbe<goblin_sbe::ZAdd>(config.key.size() + pair_member.size(),
                                           [&](auto& message) {
        message.flags(0);
        auto& entries = message.membersCount(1);
        entries.next()
            .score(static_cast<double>(zset_score(pair_item)))
            .putMember(pair_member.data(),
                       static_cast<std::uint32_t>(pair_member.size()));
        message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
      });
    }
    return;
  }
  fail("unsupported SBE ZSET action: " + config.action);
}

TimedResult run_sbe(const Config& config) {
  auto client = SbeRingClient::open(config.ring.c_str(), std::chrono::seconds(10));
  if (!client) fail("could not open SBE ring");
  const auto started = Clock::now();
  std::uint64_t checksum = 0;
  client->pipeline_for(
      static_cast<std::size_t>(command_count(config)),
      static_cast<std::size_t>(config.pipeline),
      [&](std::size_t ordinal) {
        if (config.suite == "set") {
          enqueue_set_command(*client, config, static_cast<std::int64_t>(ordinal));
        } else {
          enqueue_zset_command(*client, config, static_cast<std::int64_t>(ordinal));
        }
      },
      [&](std::size_t) {
        if (config.action == "multi-member" || config.action == "range-16") {
          const auto values = client->read_pipeline_array();
          checksum += values.size() + 47U;
        } else if (config.action == "score-hit") {
          const auto value = client->read_pipeline_double_or_nil();
          checksum += value ? 97U : 1U;
        } else if (config.action == "rank") {
          const auto value = client->read_pipeline_int_or_nil();
          checksum += static_cast<std::uint64_t>(value.value_or(-1) + 17);
        } else {
          checksum += static_cast<std::uint64_t>(client->read_pipeline_int() + 17);
        }
      });
  return {checksum, std::chrono::duration<double>(Clock::now() - started).count()};
}

int run(int argc, char** argv) {
  const Config config = parse_args(argc, argv);
  if (config.suite == "array" && config.action == "increment-latency") {
    if (config.transport != "resp") {
      fail("ARRAY increment latency supports RESP only");
    }
    const auto result = run_array_increment_latency(config);
    const auto commands = command_count(config);
    std::cout << "commands=" << commands << '\n'
              << "logical_operations=" << commands << '\n'
              << std::setprecision(17) << "seconds=" << result.timed.seconds
              << '\n'
              << "checksum=" << result.timed.checksum << '\n'
              << "latency_p90_us=" << result.p90_us << '\n'
              << "latency_p99_us=" << result.p99_us << '\n'
              << "latency_p999_us=" << result.p999_us << '\n'
              << "latency_p9999_us=" << result.p9999_us << '\n'
              << "reserved=" << (config.array_reserve_bytes != 0 ? 1 : 0)
              << '\n';
    return 0;
  }
  const auto result = config.transport == "sbe" ? run_sbe(config) : run_resp(config);
  const auto commands = command_count(config);
  const auto logical_operations = config.action == "churn" ? commands / 2 : commands;
  std::cout << "commands=" << commands << '\n'
            << "logical_operations=" << logical_operations << '\n'
            << std::setprecision(17) << "seconds=" << result.seconds << '\n'
            << "checksum=" << result.checksum << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "collection_benchmark_worker: " << error.what() << '\n';
    return 1;
  }
}
