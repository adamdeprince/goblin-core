// Populate, resume, and verify every persistent Goblin Core data type.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

enum class DataType { string, hash, set, zset, list, array, all };

constexpr std::array<DataType, 6> kPersistentTypes{
    DataType::string, DataType::hash, DataType::set,
    DataType::zset,   DataType::list, DataType::array};

struct Config {
  std::string action;
  std::string host = "127.0.0.1";
  std::string key_prefix = "external-logging-test";
  std::string snapshot = "/mnt/local/goblin-core/state/goblin.snapshot";
  DataType data_type = DataType::all;
  int port = 6379;
  std::uint64_t count = 1'000'000;
  std::uint64_t snapshot_at = 500'000;
  std::size_t pipeline = 512;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

std::string next_arg(int argc, char** argv, int& index) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + argv[index]);
  return argv[++index];
}

std::string_view data_type_name(DataType type) {
  switch (type) {
    case DataType::string:
      return "string";
    case DataType::hash:
      return "hash";
    case DataType::set:
      return "set";
    case DataType::zset:
      return "zset";
    case DataType::list:
      return "list";
    case DataType::array:
      return "array";
    case DataType::all:
      return "all";
  }
  return "unknown";
}

DataType parse_data_type(std::string_view value) {
  if (value == "string" || value == "strings") return DataType::string;
  if (value == "hash" || value == "hset") return DataType::hash;
  if (value == "set") return DataType::set;
  if (value == "zset" || value == "sorted-set") return DataType::zset;
  if (value == "list") return DataType::list;
  if (value == "array") return DataType::array;
  if (value == "all") return DataType::all;
  fail("--data-type must be string, hash, set, zset, list, array, or all");
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--action") config.action = next_arg(argc, argv, i);
    else if (arg == "--host") config.host = next_arg(argc, argv, i);
    else if (arg == "--port") config.port = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--key-prefix") config.key_prefix = next_arg(argc, argv, i);
    else if (arg == "--snapshot") config.snapshot = next_arg(argc, argv, i);
    else if (arg == "--data-type") {
      config.data_type = parse_data_type(next_arg(argc, argv, i));
    } else if (arg == "--count") {
      config.count = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--snapshot-at") {
      config.snapshot_at = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--pipeline") {
      config.pipeline = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: " << argv[0]
          << " --action prepare|continue|verify|probe [options]\n"
          << "  --data-type string|hash|set|zset|list|array|all\n"
          << "  --host HOST --port PORT --key-prefix PREFIX\n"
          << "  --count N --snapshot-at N --pipeline N --snapshot PATH\n";
      std::exit(0);
    } else {
      fail("unknown argument: " + arg);
    }
  }
  if (config.action != "prepare" && config.action != "continue" &&
      config.action != "verify" && config.action != "probe") {
    fail("--action must be prepare, continue, verify, or probe");
  }
  if (config.port < 1 || config.port > 65'535 || config.count == 0 ||
      config.pipeline == 0) {
    fail("numeric argument is out of range");
  }
  if (config.snapshot_at == 0 || config.snapshot_at > config.count) {
    fail("--snapshot-at must be in [1, --count]");
  }
  if (config.key_prefix.empty()) fail("--key-prefix must not be empty");
  return config;
}

std::vector<DataType> selected_types(DataType selected) {
  if (selected == DataType::all) {
    return {kPersistentTypes.begin(), kPersistentTypes.end()};
  }
  return {selected};
}

enum class RespType { nil, string, integer };

struct RespValue {
  RespType type = RespType::nil;
  std::string string;
  std::int64_t integer = 0;
};

std::string collection_key(std::string_view prefix, DataType type) {
  std::string result(prefix);
  result.push_back(':');
  result += data_type_name(type);
  return result;
}

std::string string_key(std::string_view prefix, std::string_view ordinal) {
  std::string result(prefix);
  result += ":string:";
  result += ordinal;
  return result;
}

class RespClient {
 public:
  struct Verification {
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::string first_failure;
  };

  RespClient(std::string_view host, int port) : input_(64U * 1024U) {
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
    const std::string host_string(host);
    if (::inet_pton(AF_INET, host_string.c_str(), &address.sin_addr) != 1) {
      fail("invalid IPv4 host: " + host_string);
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

  RespValue command(const std::vector<std::string_view>& arguments) {
    std::string output;
    output.reserve(128);
    append_command(output, arguments);
    send_all(output);
    return read_response();
  }

  std::uint64_t remove_dataset(DataType type, std::string_view prefix,
                               std::uint64_t count, std::size_t batch_size) {
    if (type != DataType::string) {
      const auto key = collection_key(prefix, type);
      return static_cast<std::uint64_t>(
          require_integer(command({"DEL", key}), "DEL"));
    }

    std::uint64_t removed = 0;
    std::array<char, 32> digits{};
    for (std::uint64_t begin = 0; begin < count;) {
      const auto end = std::min(count, begin + batch_size);
      std::string output;
      output += '*';
      output += std::to_string(end - begin + 1);
      output += "\r\n$3\r\nDEL\r\n";
      for (auto ordinal = begin; ordinal < end; ++ordinal) {
        const auto decimal = format_ordinal(ordinal, digits);
        append_bulk(output, string_key(prefix, decimal));
      }
      send_all(output);
      const auto reply = require_integer(read_response(), "DEL");
      if (reply < 0) fail("DEL returned a negative count");
      removed += static_cast<std::uint64_t>(reply);
      begin = end;
    }
    return removed;
  }

  void populate_range(DataType type, std::string_view prefix,
                      std::uint64_t begin, std::uint64_t end,
                      std::size_t pipeline, std::string_view phase) {
    const auto key = collection_key(prefix, type);
    std::array<char, 32> digits{};
    for (std::uint64_t batch_begin = begin; batch_begin < end;) {
      const auto batch_end = std::min(end, batch_begin + pipeline);
      std::string output;
      output.reserve(static_cast<std::size_t>(batch_end - batch_begin) * 72U);
      for (auto ordinal = batch_begin; ordinal < batch_end; ++ordinal) {
        const auto decimal = format_ordinal(ordinal, digits);
        append_write(output, type, prefix, key, decimal);
      }
      send_all(output);
      for (auto ordinal = batch_begin; ordinal < batch_end; ++ordinal) {
        validate_write_reply(type, ordinal, read_response());
      }
      batch_begin = batch_end;
      report_progress(phase, type, batch_begin, end);
    }
  }

  Verification verify_range(DataType type, std::string_view prefix,
                            std::uint64_t begin, std::uint64_t end,
                            std::size_t pipeline, std::string_view phase) {
    Verification result;
    const auto key = collection_key(prefix, type);
    std::array<char, 32> digits{};
    for (std::uint64_t batch_begin = begin; batch_begin < end;) {
      const auto batch_end = std::min(end, batch_begin + pipeline);
      std::string output;
      output.reserve(static_cast<std::size_t>(batch_end - batch_begin) * 56U);
      for (auto ordinal = batch_begin; ordinal < batch_end; ++ordinal) {
        const auto decimal = format_ordinal(ordinal, digits);
        append_read(output, type, prefix, key, decimal);
      }
      send_all(output);
      for (auto ordinal = batch_begin; ordinal < batch_end; ++ordinal) {
        const auto decimal = format_ordinal(ordinal, digits);
        validate_read_reply(type, decimal, read_response(), result);
      }
      batch_begin = batch_end;
      report_progress(phase, type, batch_begin, end);
    }
    return result;
  }

  void require_cardinality(DataType type, std::string_view prefix,
                           std::uint64_t expected) {
    if (type == DataType::string) return;
    const auto key = collection_key(prefix, type);
    const auto command_name = cardinality_command(type);
    const auto actual = require_integer(command({command_name, key}), command_name);
    if (actual != static_cast<std::int64_t>(expected)) {
      fail(std::string(command_name) + " for " + std::string(data_type_name(type)) +
           " is " + std::to_string(actual) + ", expected " +
           std::to_string(expected));
    }
    std::cout << "cardinality_type=" << data_type_name(type)
              << " cardinality=" << actual << '\n';
  }

 private:
  int fd_ = -1;
  std::vector<char> input_;
  std::size_t input_pos_ = 0;
  std::size_t input_size_ = 0;

  static std::string_view format_ordinal(std::uint64_t ordinal,
                                         std::array<char, 32>& digits) {
    const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                         ordinal);
    if (converted.ec != std::errc{}) fail("integer formatting failed");
    return {digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())};
  }

  static void append_bulk(std::string& output, std::string_view value) {
    output.push_back('$');
    output += std::to_string(value.size());
    output += "\r\n";
    output += value;
    output += "\r\n";
  }

  static void append_command(std::string& output,
                             const std::vector<std::string_view>& arguments) {
    output.push_back('*');
    output += std::to_string(arguments.size());
    output += "\r\n";
    for (const auto argument : arguments) append_bulk(output, argument);
  }

  static void append_write(std::string& output, DataType type,
                           std::string_view prefix, std::string_view key,
                           std::string_view decimal) {
    switch (type) {
      case DataType::string:
        output += "*3\r\n$3\r\nSET\r\n";
        append_bulk(output, string_key(prefix, decimal));
        append_bulk(output, decimal);
        return;
      case DataType::hash:
        output += "*4\r\n$4\r\nHSET\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        append_bulk(output, decimal);
        return;
      case DataType::set:
        output += "*3\r\n$4\r\nSADD\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::zset:
        output += "*4\r\n$4\r\nZADD\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        append_bulk(output, decimal);
        return;
      case DataType::list:
        output += "*3\r\n$5\r\nRPUSH\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::array:
        output += "*4\r\n$5\r\nARSET\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        append_bulk(output, decimal);
        return;
      case DataType::all:
        break;
    }
    fail("cannot populate synthetic all type");
  }

  static void append_read(std::string& output, DataType type,
                          std::string_view prefix, std::string_view key,
                          std::string_view decimal) {
    switch (type) {
      case DataType::string:
        output += "*2\r\n$3\r\nGET\r\n";
        append_bulk(output, string_key(prefix, decimal));
        return;
      case DataType::hash:
        output += "*3\r\n$4\r\nHGET\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::set:
        output += "*3\r\n$9\r\nSISMEMBER\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::zset:
        output += "*3\r\n$6\r\nZSCORE\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::list:
        output += "*3\r\n$6\r\nLINDEX\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::array:
        output += "*3\r\n$5\r\nARGET\r\n";
        append_bulk(output, key);
        append_bulk(output, decimal);
        return;
      case DataType::all:
        break;
    }
    fail("cannot verify synthetic all type");
  }

  static void validate_write_reply(DataType type, std::uint64_t ordinal,
                                   const RespValue& reply) {
    if (type == DataType::string) {
      if (reply.type != RespType::string || reply.string != "OK") {
        fail("SET failed for ordinal " + std::to_string(ordinal));
      }
      return;
    }
    if (reply.type != RespType::integer) {
      fail(std::string(data_type_name(type)) +
           " write returned a non-integer reply at ordinal " +
           std::to_string(ordinal));
    }
    const auto expected = type == DataType::list
                              ? static_cast<std::int64_t>(ordinal + 1)
                              : 1;
    if (reply.integer != expected) {
      fail(std::string(data_type_name(type)) + " write returned " +
           std::to_string(reply.integer) + " at ordinal " +
           std::to_string(ordinal) + ", expected " + std::to_string(expected));
    }
  }

  static void validate_read_reply(DataType type, std::string_view expected,
                                  const RespValue& reply,
                                  Verification& result) {
    if (reply.type == RespType::nil) {
      ++result.missing;
      remember_failure(result, "missing " + std::string(data_type_name(type)) +
                                   " ordinal " + std::string(expected));
      return;
    }
    const bool matches = type == DataType::set
                             ? reply.type == RespType::integer && reply.integer == 1
                             : reply.type == RespType::string &&
                                   reply.string == expected;
    if (!matches) {
      ++result.mismatched;
      remember_failure(result, "wrong " + std::string(data_type_name(type)) +
                                   " value at ordinal " + std::string(expected));
      return;
    }
    ++result.matched;
  }

  static std::string_view cardinality_command(DataType type) {
    switch (type) {
      case DataType::hash:
        return "HLEN";
      case DataType::set:
        return "SCARD";
      case DataType::zset:
        return "ZCARD";
      case DataType::list:
        return "LLEN";
      case DataType::array:
        return "ARLEN";
      case DataType::string:
      case DataType::all:
        break;
    }
    fail("strings do not have a single cardinality command");
  }

  static void report_progress(std::string_view phase, DataType type,
                              std::uint64_t current, std::uint64_t end) {
    if (current == end || current % 100'000 < 512) {
      std::cout << "progress_phase=" << phase << " data_type="
                << data_type_name(type) << " entries=" << current << '/' << end
                << '\n'
                << std::flush;
    }
  }

  static void remember_failure(Verification& result, std::string message) {
    if (result.first_failure.empty()) result.first_failure = std::move(message);
  }

  static std::int64_t require_integer(const RespValue& reply,
                                      std::string_view operation) {
    if (reply.type != RespType::integer) {
      fail(std::string(operation) + " returned a non-integer reply");
    }
    return reply.integer;
  }

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
      return RespValue{RespType::string, std::move(line), 0};
    }
    if (prefix == ':') {
      return RespValue{RespType::integer, {}, parse_integer(read_line())};
    }
    if (prefix == '$') {
      const auto length = parse_integer(read_line());
      if (length < 0) return {};
      auto value = read_bytes(static_cast<std::size_t>(length));
      if (read_bytes(2) != "\r\n") fail("invalid RESP bulk terminator");
      return RespValue{RespType::string, std::move(value), 0};
    }
    fail(std::string("unsupported RESP reply prefix: ") + prefix);
  }
};

double elapsed_seconds(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}

void print_rate(std::string_view phase, DataType type, std::uint64_t entries,
                double seconds) {
  std::cout << std::fixed << std::setprecision(6) << "phase=" << phase
            << " data_type=" << data_type_name(type) << " entries=" << entries
            << " seconds=" << seconds << " operations_per_second="
            << (seconds == 0.0 ? 0.0 : static_cast<double>(entries) / seconds)
            << '\n';
}

void require_verification(DataType type, std::uint64_t expected,
                          const RespClient::Verification& result,
                          std::string_view phase) {
  std::cout << "phase=" << phase << " data_type=" << data_type_name(type)
            << " matched=" << result.matched << " missing=" << result.missing
            << " mismatched=" << result.mismatched << '\n';
  if (!result.first_failure.empty()) {
    std::cout << "first_failure=" << result.first_failure << '\n';
  }
  if (result.matched != expected || result.missing != 0 ||
      result.mismatched != 0) {
    fail(std::string(phase) + " verification failed for " +
         std::string(data_type_name(type)));
  }
}

void prepare(const Config& config) {
  RespClient client(config.host, config.port);
  const auto types = selected_types(config.data_type);
  for (const auto type : types) {
    const auto removed =
        client.remove_dataset(type, config.key_prefix, config.count, config.pipeline);
    std::cout << "cleanup_data_type=" << data_type_name(type)
              << " removed=" << removed << '\n';
  }

  const auto total_begin = Clock::now();
  for (const auto type : types) {
    const auto begin = Clock::now();
    client.populate_range(type, config.key_prefix, 0, config.snapshot_at,
                          config.pipeline, "prepare");
    client.require_cardinality(type, config.key_prefix, config.snapshot_at);
    print_rate("prepare", type, config.snapshot_at, elapsed_seconds(begin));
  }

  const auto save_reply =
      client.command({"GOBLIN.SAVE", config.snapshot, "ACCEL"});
  if (save_reply.type != RespType::string ||
      save_reply.string != "Background saving started") {
    fail("GOBLIN.SAVE returned an unexpected reply");
  }
  std::cout << "snapshot_reply=" << save_reply.string << '\n'
            << "snapshot_entries_per_type=" << config.snapshot_at << '\n'
            << "prepare_total_seconds=" << elapsed_seconds(total_begin) << '\n';
}

void continue_after_recovery(const Config& config) {
  RespClient client(config.host, config.port);
  const auto types = selected_types(config.data_type);
  for (const auto type : types) {
    client.require_cardinality(type, config.key_prefix, config.snapshot_at);
    const auto begin = Clock::now();
    const auto recovered = client.verify_range(
        type, config.key_prefix, 0, config.snapshot_at, config.pipeline,
        "recovered-half");
    require_verification(type, config.snapshot_at, recovered, "recovered-half");
    print_rate("recovered-half-read", type, config.snapshot_at,
               elapsed_seconds(begin));
  }

  for (const auto type : types) {
    const auto begin = Clock::now();
    client.populate_range(type, config.key_prefix, config.snapshot_at,
                          config.count, config.pipeline, "continue");
    client.require_cardinality(type, config.key_prefix, config.count);
    print_rate("continue", type, config.count - config.snapshot_at,
               elapsed_seconds(begin));
  }
}

void verify(const Config& config) {
  RespClient client(config.host, config.port);
  const auto types = selected_types(config.data_type);
  for (const auto type : types) {
    client.require_cardinality(type, config.key_prefix, config.count);
    const auto begin = Clock::now();
    const auto result = client.verify_range(type, config.key_prefix, 0,
                                            config.count, config.pipeline,
                                            "final");
    require_verification(type, config.count, result, "final");
    print_rate("final-read", type, config.count, elapsed_seconds(begin));
  }
}

void probe(const Config& config) {
  RespClient client(config.host, config.port);
  const auto reply = client.command({"PING"});
  if (reply.type != RespType::string || reply.string != "PONG") {
    fail("PING returned an unexpected reply");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = parse_args(argc, argv);
    if (config.action == "prepare") prepare(config);
    else if (config.action == "continue") continue_after_recovery(config);
    else if (config.action == "verify") verify(config);
    else probe(config);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "external_logging_recovery: " << error.what() << '\n';
    return 1;
  }
}
