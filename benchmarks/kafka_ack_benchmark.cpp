// End-to-end latency and throughput benchmark for Goblin's Kafka write
// acknowledgement policies. The client always uses RESP2 over a Unix socket;
// only the server's durability boundary changes between runs.

#include "goblin/core/ring_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Config {
  std::string server;
  std::string mode;
  std::string brokers;
  std::string topic;
  std::string socket_path;
  std::string server_log;
  std::size_t warmup{2'000};
  std::size_t latency_samples{100'000};
  std::size_t pipeline_operations{200'000};
  std::size_t transaction_operations{50'000};
  std::vector<std::size_t> pipeline_depths{8, 32, 128, 512};
  std::vector<std::size_t> transaction_sizes{1, 8, 32, 128};
  std::size_t transaction_buffer_bytes{64U * 1024U};
  int server_core{0};
  int client_core{4};
  int linger_ms{0};
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

std::string next_arg(int argc, char** argv, int& index) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + argv[index]);
  return argv[++index];
}

std::vector<std::size_t> parse_sizes(std::string_view text) {
  std::vector<std::size_t> values;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto comma = text.find(',', begin);
    const auto part = text.substr(
        begin, comma == std::string_view::npos ? text.size() - begin
                                               : comma - begin);
    std::size_t value = 0;
    const auto [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), value);
    if (error != std::errc{} || end != part.data() + part.size() || value == 0) {
      fail("invalid comma-separated size list");
    }
    values.push_back(value);
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
  }
  return values;
}

std::size_t parse_size(std::string_view text, std::string_view option) {
  std::size_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
    fail("invalid value for " + std::string(option));
  }
  return value;
}

void print_help(const char* program) {
  std::cout
      << "Usage: " << program << " --server PATH --mode none|queued|broker [options]\n"
      << "  --brokers HOST:PORT          required for queued/broker\n"
      << "  --topic TOPIC                required for queued/broker; must be empty\n"
      << "  --socket PATH                default /tmp/goblin-kafka-ack-<pid>.sock\n"
      << "  --server-log PATH            child stdout/stderr\n"
      << "  --warmup N                   default 2000\n"
      << "  --latency-samples N          default 100000\n"
      << "  --pipeline-operations N      per depth; default 200000\n"
      << "  --pipeline-depths CSV        default 8,32,128,512\n"
      << "  --transaction-operations N   per size; default 50000 writes\n"
      << "  --transaction-sizes CSV      default 1,8,32,128\n"
      << "  --transaction-buffer-bytes N server buffer; default 65536\n"
      << "  --server-core N              default 0\n"
      << "  --client-core N              default 4\n"
      << "  --linger-ms N                Kafka producer linger; default 0\n";
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      print_help(argv[0]);
      std::exit(0);
    } else if (arg == "--server") {
      config.server = next_arg(argc, argv, i);
    } else if (arg == "--mode") {
      config.mode = next_arg(argc, argv, i);
    } else if (arg == "--brokers") {
      config.brokers = next_arg(argc, argv, i);
    } else if (arg == "--topic") {
      config.topic = next_arg(argc, argv, i);
    } else if (arg == "--socket") {
      config.socket_path = next_arg(argc, argv, i);
    } else if (arg == "--server-log") {
      config.server_log = next_arg(argc, argv, i);
    } else if (arg == "--warmup") {
      config.warmup = parse_size(next_arg(argc, argv, i), arg);
    } else if (arg == "--latency-samples") {
      config.latency_samples = parse_size(next_arg(argc, argv, i), arg);
    } else if (arg == "--pipeline-operations") {
      config.pipeline_operations = parse_size(next_arg(argc, argv, i), arg);
    } else if (arg == "--pipeline-depths") {
      config.pipeline_depths = parse_sizes(next_arg(argc, argv, i));
    } else if (arg == "--transaction-operations") {
      config.transaction_operations = parse_size(next_arg(argc, argv, i), arg);
    } else if (arg == "--transaction-sizes") {
      config.transaction_sizes = parse_sizes(next_arg(argc, argv, i));
    } else if (arg == "--transaction-buffer-bytes") {
      config.transaction_buffer_bytes =
          parse_size(next_arg(argc, argv, i), arg);
    } else if (arg == "--server-core") {
      config.server_core = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--client-core") {
      config.client_core = std::stoi(next_arg(argc, argv, i));
    } else if (arg == "--linger-ms") {
      config.linger_ms = std::stoi(next_arg(argc, argv, i));
      if (config.linger_ms < 0) fail("--linger-ms cannot be negative");
    } else {
      fail("unknown option: " + std::string(arg));
    }
  }
  if (config.server.empty()) fail("--server is required");
  if (config.mode != "none" && config.mode != "queued" &&
      config.mode != "broker") {
    fail("--mode must be none, queued, or broker");
  }
  if (config.mode != "none" &&
      (config.brokers.empty() || config.topic.empty())) {
    fail("--brokers and --topic are required for Kafka modes");
  }
  if (config.socket_path.empty()) {
    config.socket_path =
        "/tmp/goblin-kafka-ack-" + std::to_string(::getpid()) + ".sock";
  }
  if (config.server_log.empty()) {
    config.server_log =
        "/tmp/goblin-kafka-ack-" + std::to_string(::getpid()) + ".log";
  }
  return config;
}

void pin_to_core(int core) {
#ifdef __linux__
  if (core < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    fail("cannot pin benchmark client to CPU " + std::to_string(core) + ": " +
         std::strerror(errno));
  }
#else
  (void)core;
#endif
}

std::uint16_t reserve_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) fail("cannot reserve loopback TCP port");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
      0) {
    (void)::close(fd);
    fail("cannot reserve loopback TCP port");
  }
  socklen_t size = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    (void)::close(fd);
    fail("cannot read reserved loopback TCP port");
  }
  const auto port = ntohs(address.sin_port);
  (void)::close(fd);
  return port;
}

class ServerProcess {
 public:
  explicit ServerProcess(const Config& config) : socket_path_(config.socket_path) {
    (void)::unlink(socket_path_.c_str());
    const auto port = std::to_string(reserve_loopback_port());
    pid_ = ::fork();
    if (pid_ < 0) fail("fork failed");
    if (pid_ != 0) return;

#ifdef __linux__
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    const int log = ::open(config.server_log.c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log >= 0) {
      (void)::dup2(log, STDOUT_FILENO);
      (void)::dup2(log, STDERR_FILENO);
      (void)::close(log);
    }
    std::vector<std::string> arguments{
        config.server,       "--uds-listen", socket_path_,
        "--port",           port,           "--cpu",
        std::to_string(config.server_core), "--transaction-buffer-bytes",
        std::to_string(config.transaction_buffer_bytes)};
    if (config.mode != "none") {
      arguments.emplace_back("--kafka");
      arguments.emplace_back("kafka://" + config.brokers + '/' + config.topic +
                             "?linger.ms=" + std::to_string(config.linger_ms));
      arguments.emplace_back("--kafka-ack-mode");
      arguments.emplace_back(config.mode);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(config.server.c_str(), argv.data());
    _exit(127);
  }

  ~ServerProcess() {
    stop();
    (void)::unlink(socket_path_.c_str());
  }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  [[nodiscard]] bool running() noexcept {
    if (pid_ <= 0) return false;
    int status = 0;
    const auto result = ::waitpid(pid_, &status, WNOHANG);
    if (result == 0) return true;
    if (result == pid_ || (result < 0 && errno == ECHILD)) pid_ = -1;
    return false;
  }

 private:
  void stop() noexcept {
    if (pid_ <= 0) return;
    (void)::kill(pid_, SIGTERM);
    for (int attempt = 0; attempt < 100; ++attempt) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(pid_, SIGKILL);
    (void)::waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }

  pid_t pid_{-1};
  std::string socket_path_;
};

class Client {
 public:
  Client(const std::string& path, ServerProcess& server) {
    for (int attempt = 0; attempt < 3'000; ++attempt) {
      fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd_ < 0) fail("socket failed");
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) fail("UDS path is too long");
      std::memcpy(address.sun_path, path.data(), path.size());
      if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0) {
        return;
      }
      (void)::close(fd_);
      fd_ = -1;
      if (!server.running()) fail("goblin-core exited during startup");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fail("goblin-core did not open its UDS listener within 30 seconds");
  }

  ~Client() {
    if (fd_ >= 0) (void)::close(fd_);
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void send_all(std::string_view bytes) {
    while (!bytes.empty()) {
      const auto sent = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR) continue;
      if (sent <= 0) fail("send failed: " + std::string(std::strerror(errno)));
      bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
  }

  std::string read_reply() {
    std::array<char, 64U * 1024U> buffer{};
    for (;;) {
      const auto available = std::string_view(pending_).substr(offset_);
      if (const auto end = goblin::core::ring::reply_end(available)) {
        std::string reply(available.substr(0, *end));
        offset_ += *end;
        if (offset_ >= 64U * 1024U && offset_ * 2 >= pending_.size()) {
          pending_.erase(0, offset_);
          offset_ = 0;
        }
        return reply;
      }
      pollfd event{.fd = fd_, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&event, 1, 60'000);
      if (ready <= 0) fail(ready == 0 ? "reply timed out" : "poll failed");
      const auto received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received <= 0) fail("connection closed while reading reply");
      pending_.append(buffer.data(), static_cast<std::size_t>(received));
    }
  }

  std::string command(std::span<const std::string_view> fields) {
    send_all(goblin::core::ring::encode_command(fields));
    return read_reply();
  }

  void expect_replies(std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      const auto reply = read_reply();
      if (!reply.empty() && reply.front() == '-') {
        fail("server returned " + reply);
      }
    }
  }

 private:
  int fd_{-1};
  std::string pending_;
  std::size_t offset_{0};
};

std::optional<std::int64_t> info_integer(std::string_view reply,
                                         std::string_view field) {
  const std::string needle = std::string(field) + ':';
  const auto position = reply.find(needle);
  if (position == std::string_view::npos) return std::nullopt;
  const auto begin = position + needle.size();
  const auto end = reply.find("\r\n", begin);
  if (end == std::string_view::npos) return std::nullopt;
  std::int64_t value = 0;
  const auto [parsed, error] =
      std::from_chars(reply.data() + begin, reply.data() + end, value);
  if (error != std::errc{} || parsed != reply.data() + end) return std::nullopt;
  return value;
}

std::string info(Client& client) {
  constexpr std::array<std::string_view, 1> command{"INFO"};
  return client.command(command);
}

struct JournalDrain {
  std::int64_t pending_after_client{-1};
  double seconds{0.0};
};

JournalDrain wait_for_journal(Client& client, std::string_view mode) {
  if (mode == "none") return {};
  const auto start = Clock::now();
  const auto deadline = Clock::now() + std::chrono::seconds(120);
  std::int64_t initial_pending = -1;
  while (Clock::now() < deadline) {
    const auto response = info(client);
    const auto pending = info_integer(response, "kafka_pending_records");
    if (initial_pending < 0 && pending) initial_pending = *pending;
    const auto acknowledged =
        info_integer(response, "kafka_acknowledged_logical_offset");
    const auto applied = info_integer(response, "master_repl_offset");
    if (pending == 0 && acknowledged && applied && *acknowledged == *applied) {
      return JournalDrain{
          .pending_after_client = initial_pending,
          .seconds = std::chrono::duration<double>(Clock::now() - start).count()};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  fail("Kafka journal did not drain within 120 seconds");
}

struct Distribution {
  double mean_us{0.0};
  double p50_us{0.0};
  double p75_us{0.0};
  double p90_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double p999_us{0.0};
  double p9999_us{0.0};
  double max_us{0.0};
};

Distribution summarize(std::vector<std::uint64_t> values) {
  if (values.empty()) fail("cannot summarize an empty sample");
  std::sort(values.begin(), values.end());
  long double total = 0;
  for (const auto value : values) total += value;
  const auto percentile = [&](long double fraction) {
    const auto index = static_cast<std::size_t>(
        std::floor(fraction * static_cast<long double>(values.size() - 1)));
    return static_cast<double>(values[index]) / 1'000.0;
  };
  return Distribution{
      .mean_us = static_cast<double>(total / values.size()) / 1'000.0,
      .p50_us = percentile(0.50L),
      .p75_us = percentile(0.75L),
      .p90_us = percentile(0.90L),
      .p95_us = percentile(0.95L),
      .p99_us = percentile(0.99L),
      .p999_us = percentile(0.999L),
      .p9999_us = percentile(0.9999L),
      .max_us = static_cast<double>(values.back()) / 1'000.0};
}

struct Result {
  std::string workload;
  std::size_t operations{0};
  std::size_t batch_size{0};
  std::size_t observations{0};
  double seconds{0.0};
  JournalDrain journal_drain;
  Distribution latency;
};

Result run_sequential(Client& client, std::size_t operations) {
  const std::string a = goblin::core::ring::encode_command(
      std::array<std::string_view, 3>{"SET", "kafka-ack:latency", "aaaaaaaaaaaaaaaa"});
  const std::string b = goblin::core::ring::encode_command(
      std::array<std::string_view, 3>{"SET", "kafka-ack:latency", "bbbbbbbbbbbbbbbb"});
  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(operations);
  const auto whole_start = Clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    const auto start = Clock::now();
    client.send_all((index & 1U) == 0 ? a : b);
    client.expect_replies(1);
    elapsed.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
            .count()));
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - whole_start).count();
  return Result{.workload = "sequential_set",
                .operations = operations,
                .batch_size = 1,
                .observations = operations,
                .seconds = seconds,
                .journal_drain = {},
                .latency = summarize(std::move(elapsed))};
}

Result run_pipeline(Client& client, std::size_t requested_operations,
                    std::size_t depth) {
  const std::string a = goblin::core::ring::encode_command(
      std::array<std::string_view, 3>{"SET", "kafka-ack:pipeline", "aaaaaaaaaaaaaaaa"});
  const std::string b = goblin::core::ring::encode_command(
      std::array<std::string_view, 3>{"SET", "kafka-ack:pipeline", "bbbbbbbbbbbbbbbb"});
  std::string batch;
  batch.reserve(depth * std::max(a.size(), b.size()));
  for (std::size_t index = 0; index < depth; ++index) {
    batch.append((index & 1U) == 0 ? a : b);
  }
  const auto batches = std::max<std::size_t>(requested_operations / depth, 1);
  const auto operations = batches * depth;
  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(batches);
  const auto whole_start = Clock::now();
  for (std::size_t index = 0; index < batches; ++index) {
    const auto start = Clock::now();
    client.send_all(batch);
    client.expect_replies(depth);
    elapsed.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
            .count()));
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - whole_start).count();
  return Result{.workload = "pipelined_set",
                .operations = operations,
                .batch_size = depth,
                .observations = batches,
                .seconds = seconds,
                .journal_drain = {},
                .latency = summarize(std::move(elapsed))};
}

std::string transaction_wire(std::size_t writes, std::string_view value) {
  std::string wire = goblin::core::ring::encode_command(
      std::array<std::string_view, 1>{"MULTI"});
  for (std::size_t index = 0; index < writes; ++index) {
    const std::string key = "kafka-ack:transaction:" + std::to_string(index);
    wire.append(goblin::core::ring::encode_command(
        std::array<std::string_view, 3>{"SET", key, value}));
  }
  wire.append(goblin::core::ring::encode_command(
      std::array<std::string_view, 1>{"EXEC"}));
  return wire;
}

Result run_transactions(Client& client, std::size_t requested_operations,
                        std::size_t writes) {
  const auto a = transaction_wire(writes, "aaaaaaaaaaaaaaaa");
  const auto b = transaction_wire(writes, "bbbbbbbbbbbbbbbb");
  const auto transactions =
      std::max<std::size_t>(requested_operations / writes, 1);
  const auto operations = transactions * writes;
  std::vector<std::uint64_t> elapsed;
  elapsed.reserve(transactions);
  const auto whole_start = Clock::now();
  for (std::size_t index = 0; index < transactions; ++index) {
    const auto start = Clock::now();
    client.send_all((index & 1U) == 0 ? a : b);
    client.expect_replies(writes + 2);
    elapsed.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
            .count()));
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - whole_start).count();
  return Result{.workload = "transaction_set_exec",
                .operations = operations,
                .batch_size = writes,
                .observations = transactions,
                .seconds = seconds,
                .journal_drain = {},
                .latency = summarize(std::move(elapsed))};
}

void print_result(std::string_view mode, const Result& result) {
  const double qps = static_cast<double>(result.operations) / result.seconds;
  const double settled_qps = static_cast<double>(result.operations) /
                             (result.seconds + result.journal_drain.seconds);
  std::cout << mode << ',' << result.workload << ',' << result.operations << ','
            << result.batch_size << ',' << result.observations << ',' << std::fixed
            << std::setprecision(6) << result.seconds << ','
            << std::setprecision(2) << qps << ','
            << result.journal_drain.pending_after_client << ','
            << std::setprecision(6) << result.journal_drain.seconds << ','
            << std::setprecision(2) << settled_qps << ','
            << result.latency.mean_us << ','
            << result.latency.p50_us << ',' << result.latency.p75_us << ','
            << result.latency.p90_us << ',' << result.latency.p95_us << ','
            << result.latency.p99_us << ',' << result.latency.p999_us << ','
            << result.latency.p9999_us << ',' << result.latency.max_us << '\n';
  std::cout.flush();
}

void warm_up(Client& client, std::size_t operations) {
  const auto command = goblin::core::ring::encode_command(
      std::array<std::string_view, 3>{"SET", "kafka-ack:warmup", "warm"});
  for (std::size_t index = 0; index < operations; ++index) {
    client.send_all(command);
    client.expect_replies(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = parse_args(argc, argv);
    pin_to_core(config.client_core);
    ServerProcess server(config);
    Client client(config.socket_path, server);
    warm_up(client, config.warmup);
    (void)wait_for_journal(client, config.mode);

    std::cout << "mode,workload,operations,batch_size,observations,seconds,"
                 "operations_per_second,kafka_pending_after_client,"
                 "journal_drain_seconds,settled_operations_per_second,"
                 "mean_batch_us,p50_batch_us,p75_batch_us,"
                 "p90_batch_us,p95_batch_us,p99_batch_us,p99.9_batch_us,"
                 "p99.99_batch_us,max_batch_us\n";

    auto result = run_sequential(client, config.latency_samples);
    result.journal_drain = wait_for_journal(client, config.mode);
    print_result(config.mode, result);
    for (const auto depth : config.pipeline_depths) {
      result = run_pipeline(client, config.pipeline_operations, depth);
      result.journal_drain = wait_for_journal(client, config.mode);
      print_result(config.mode, result);
    }
    for (const auto size : config.transaction_sizes) {
      result = run_transactions(client, config.transaction_operations, size);
      result.journal_drain = wait_for_journal(client, config.mode);
      print_result(config.mode, result);
    }

    const auto final_info = info(client);
    const auto pending = info_integer(final_info, "kafka_pending_records");
    const auto acknowledged =
        info_integer(final_info, "kafka_acknowledged_logical_offset");
    const auto applied = info_integer(final_info, "master_repl_offset");
    std::cerr << "mode=" << config.mode << " final_replication_offset="
              << (applied ? std::to_string(*applied) : "missing")
              << " kafka_acknowledged_logical_offset="
              << (acknowledged ? std::to_string(*acknowledged) : "off")
              << " kafka_pending_records="
              << (pending ? std::to_string(*pending) : "off") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "kafka_ack_benchmark: " << error.what() << '\n';
    return 1;
  }
}
