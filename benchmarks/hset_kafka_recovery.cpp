// Populate and verify the million-field HSET Kafka crash-recovery workload.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
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

struct Config {
  std::string action;
  std::string host = "127.0.0.1";
  std::string key = "foo";
  std::string snapshot = "/mnt/local/goblin-core/state/goblin.snapshot";
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

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--action") config.action = next_arg(argc, argv, i);
    else if (arg == "--host") config.host = next_arg(argc, argv, i);
    else if (arg == "--port") config.port = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--key") config.key = next_arg(argc, argv, i);
    else if (arg == "--snapshot") config.snapshot = next_arg(argc, argv, i);
    else if (arg == "--count") config.count = std::stoull(next_arg(argc, argv, i));
    else if (arg == "--snapshot-at") {
      config.snapshot_at = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--pipeline") {
      config.pipeline = std::stoull(next_arg(argc, argv, i));
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: " << argv[0] << " --action populate|verify [options]\n"
          << "  --host HOST --port PORT --key KEY --count N --pipeline N\n"
          << "  --snapshot-at N --snapshot PATH (populate only)\n";
      std::exit(0);
    } else {
      fail("unknown argument: " + arg);
    }
  }
  if (config.action != "populate" && config.action != "verify") {
    fail("--action must be populate or verify");
  }
  if (config.port < 1 || config.port > 65'535 || config.count == 0 ||
      config.pipeline == 0) {
    fail("numeric argument is out of range");
  }
  if (config.action == "populate" && config.snapshot_at > config.count) {
    fail("--snapshot-at must not exceed --count");
  }
  return config;
}

enum class RespType { nil, string, integer };

struct RespValue {
  RespType type = RespType::nil;
  std::string string;
  std::int64_t integer = 0;
};

class RespClient {
 public:
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

  void populate_range(std::string_view key, std::uint64_t begin,
                      std::uint64_t end, std::size_t pipeline) {
    std::array<char, 32> digits{};
    for (std::uint64_t batch_begin = begin; batch_begin < end;) {
      const auto batch_end = std::min(end, batch_begin + pipeline);
      std::string output;
      output.reserve(static_cast<std::size_t>(batch_end - batch_begin) * 64U);
      for (auto value = batch_begin; value < batch_end; ++value) {
        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                             value);
        if (converted.ec != std::errc{}) fail("integer formatting failed");
        const std::string_view decimal(digits.data(),
                                       static_cast<std::size_t>(converted.ptr - digits.data()));
        append_hset(output, key, decimal);
      }
      send_all(output);
      for (auto value = batch_begin; value < batch_end; ++value) {
        const auto reply = read_response();
        if (reply.type != RespType::integer || reply.integer != 1) {
          fail("HSET did not add field " + std::to_string(value));
        }
      }
      batch_begin = batch_end;
      report_progress("populated", batch_begin, end);
    }
  }

  struct Verification {
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::string first_failure;
  };

  Verification verify_range(std::string_view key, std::uint64_t count,
                            std::size_t pipeline) {
    Verification result;
    std::array<char, 32> digits{};
    for (std::uint64_t batch_begin = 0; batch_begin < count;) {
      const auto batch_end = std::min(count, batch_begin + pipeline);
      std::string output;
      output.reserve(static_cast<std::size_t>(batch_end - batch_begin) * 48U);
      for (auto value = batch_begin; value < batch_end; ++value) {
        const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(),
                                             value);
        if (converted.ec != std::errc{}) fail("integer formatting failed");
        const std::string_view decimal(digits.data(),
                                       static_cast<std::size_t>(converted.ptr - digits.data()));
        append_hget(output, key, decimal);
      }
      send_all(output);
      for (auto value = batch_begin; value < batch_end; ++value) {
        const auto reply = read_response();
        const std::string expected = std::to_string(value);
        if (reply.type == RespType::nil) {
          ++result.missing;
          remember_failure(result, "missing field " + expected);
        } else if (reply.type != RespType::string || reply.string != expected) {
          ++result.mismatched;
          remember_failure(result, "wrong value for field " + expected);
        } else {
          ++result.matched;
        }
      }
      batch_begin = batch_end;
      report_progress("verified", batch_begin, count);
    }
    return result;
  }

 private:
  int fd_ = -1;
  std::vector<char> input_;
  std::size_t input_pos_ = 0;
  std::size_t input_size_ = 0;

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

  static void append_hset(std::string& output, std::string_view key,
                          std::string_view decimal) {
    output += "*4\r\n$4\r\nHSET\r\n";
    append_bulk(output, key);
    append_bulk(output, decimal);
    append_bulk(output, decimal);
  }

  static void append_hget(std::string& output, std::string_view key,
                          std::string_view field) {
    output += "*3\r\n$4\r\nHGET\r\n";
    append_bulk(output, key);
    append_bulk(output, field);
  }

  static void report_progress(std::string_view verb, std::uint64_t current,
                              std::uint64_t end) {
    if (current == end || current % 100'000 < 512) {
      std::cout << verb << '=' << current << '/' << end << '\n' << std::flush;
    }
  }

  static void remember_failure(Verification& result, std::string message) {
    if (result.first_failure.empty()) result.first_failure = std::move(message);
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

std::int64_t require_integer(const RespValue& reply, std::string_view operation) {
  if (reply.type != RespType::integer) {
    fail(std::string(operation) + " returned a non-integer reply");
  }
  return reply.integer;
}

void populate(const Config& config) {
  RespClient client(config.host, config.port);
  const auto removed = require_integer(client.command({"DEL", config.key}), "DEL");
  std::cout << "deleted_existing_key=" << removed << '\n';

  const auto begin = Clock::now();
  client.populate_range(config.key, 0, config.snapshot_at, config.pipeline);
  const auto save_reply = client.command({"GOBLIN.SAVE", config.snapshot, "ACCEL"});
  if (save_reply.type != RespType::string ||
      save_reply.string != "Background saving started") {
    fail("GOBLIN.SAVE returned an unexpected reply");
  }
  const double before_snapshot_seconds = elapsed_seconds(begin);
  std::cout << "snapshot_requested_before_field=" << config.snapshot_at << '\n'
            << "snapshot_reply=" << save_reply.string << '\n';

  const auto second_half_begin = Clock::now();
  client.populate_range(config.key, config.snapshot_at, config.count,
                        config.pipeline);
  const double second_half_seconds = elapsed_seconds(second_half_begin);
  const auto hlen = require_integer(client.command({"HLEN", config.key}), "HLEN");
  if (hlen != static_cast<std::int64_t>(config.count)) {
    fail("HLEN before crash is " + std::to_string(hlen) + ", expected " +
         std::to_string(config.count));
  }
  std::cout << "pre_crash_hlen=" << hlen << '\n'
            << "first_half_and_save_seconds=" << before_snapshot_seconds << '\n'
            << "second_half_seconds=" << second_half_seconds << '\n'
            << "populate_total_seconds=" << elapsed_seconds(begin) << '\n';
}

void verify(const Config& config) {
  RespClient client(config.host, config.port);
  const auto hlen = require_integer(client.command({"HLEN", config.key}), "HLEN");
  std::cout << "recovered_hlen=" << hlen << '\n';

  const auto begin = Clock::now();
  const auto result = client.verify_range(config.key, config.count, config.pipeline);
  std::cout << "matched=" << result.matched << '\n'
            << "missing=" << result.missing << '\n'
            << "mismatched=" << result.mismatched << '\n'
            << "verify_seconds=" << elapsed_seconds(begin) << '\n';
  if (!result.first_failure.empty()) {
    std::cout << "first_failure=" << result.first_failure << '\n';
  }
  if (hlen != static_cast<std::int64_t>(config.count) || result.matched != config.count ||
      result.missing != 0 || result.mismatched != 0) {
    fail("recovered HSET verification failed");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = parse_args(argc, argv);
    if (config.action == "populate") populate(config);
    else verify(config);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hset_kafka_recovery: " << error.what() << '\n';
    return 1;
  }
}
