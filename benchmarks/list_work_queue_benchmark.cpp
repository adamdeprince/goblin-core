#include "goblin/core/ring_client.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Fields = std::vector<std::string_view>;
using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

class Client {
 public:
  explicit Client(const std::string& path) {
    for (int attempt = 0; attempt < 500; ++attempt) {
      fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd_ < 0) {
        throw std::runtime_error("socket failed");
      }
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("UDS path is too long");
      }
      std::memcpy(address.sun_path, path.data(), path.size());
      if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0) {
        return;
      }
      (void)::close(fd_);
      fd_ = -1;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("server did not open its UDS listener");
  }

  ~Client() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void command(std::span<const std::string_view> fields) {
    const auto wire = encode_command(fields);
    send_all(wire);
    read_replies(1);
  }

  void repeated(std::span<const std::string_view> fields,
                std::size_t operations, std::size_t pipeline) {
    const auto one = encode_command(fields);
    std::string full;
    full.reserve(one.size() * pipeline);
    for (std::size_t index = 0; index < pipeline; ++index) {
      full.append(one);
    }
    const auto remainder = operations % pipeline;
    std::string tail;
    tail.reserve(one.size() * remainder);
    for (std::size_t index = 0; index < remainder; ++index) {
      tail.append(one);
    }

    std::size_t completed = 0;
    while (completed + pipeline <= operations) {
      send_all(full);
      read_replies(pipeline);
      completed += pipeline;
    }
    if (remainder != 0) {
      send_all(tail);
      read_replies(remainder);
    }
  }

  void populate(std::string_view key, std::size_t values,
                std::size_t pipeline) {
    constexpr std::size_t kValuesPerCommand = 128;
    const std::string value = "0123456789abcdef";
    std::vector<std::string_view> fields;
    fields.reserve(kValuesPerCommand + 2);
    std::vector<std::string> wires;
    wires.reserve((values + kValuesPerCommand - 1) / kValuesPerCommand);
    for (std::size_t first = 0; first < values;
         first += kValuesPerCommand) {
      const auto count = std::min(kValuesPerCommand, values - first);
      fields.clear();
      fields.emplace_back("RPUSH");
      fields.emplace_back(key);
      for (std::size_t index = 0; index < count; ++index) {
        fields.emplace_back(value);
      }
      wires.emplace_back(encode_command(fields));
    }
    for (std::size_t first = 0; first < wires.size(); first += pipeline) {
      const auto count = std::min(pipeline, wires.size() - first);
      std::string batch;
      for (std::size_t index = 0; index < count; ++index) {
        batch.append(wires[first + index]);
      }
      send_all(batch);
      read_replies(count);
    }
  }

 private:
  void send_all(std::string_view bytes) {
    while (!bytes.empty()) {
      const auto sent = ::send(fd_, bytes.data(), bytes.size(), 0);
      if (sent <= 0) {
        throw std::runtime_error("send failed");
      }
      bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
  }

  void read_replies(std::size_t count) {
    std::array<char, 256 * 1024> buffer{};
    while (count != 0) {
      const auto available = std::string_view(pending_).substr(pending_offset_);
      if (const auto end = reply_end(available)) {
        pending_offset_ += *end;
        --count;
        if (pending_offset_ >= 64 * 1024 &&
            pending_offset_ * 2 >= pending_.size()) {
          pending_.erase(0, pending_offset_);
          pending_offset_ = 0;
        }
        continue;
      }
      const auto received = ::recv(fd_, buffer.data(), buffer.size(), 0);
      if (received <= 0) {
        throw std::runtime_error("receive failed");
      }
      pending_.append(buffer.data(), static_cast<std::size_t>(received));
    }
  }

  int fd_{-1};
  std::string pending_;
  std::size_t pending_offset_{0};
};

class ServerProcess {
 public:
  ServerProcess(const std::string& binary, std::string implementation,
                std::string socket_path)
      : socket_path_(std::move(socket_path)) {
    (void)::unlink(socket_path_.c_str());
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("fork failed");
    }
    if (pid_ == 0) {
      const int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        (void)::dup2(devnull, STDOUT_FILENO);
        (void)::dup2(devnull, STDERR_FILENO);
      }
      ::execl(binary.c_str(), binary.c_str(), "--unixsocket",
              socket_path_.c_str(), "--list-implementation",
              implementation.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
  }

  ~ServerProcess() {
    if (pid_ > 0) {
      (void)::kill(pid_, SIGTERM);
      (void)::waitpid(pid_, nullptr, 0);
    }
    (void)::unlink(socket_path_.c_str());
  }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

 private:
  pid_t pid_{-1};
  std::string socket_path_;
};

struct Workload {
  std::string name;
  Fields command;
  std::size_t initial_values{0};
  std::size_t values_per_operation{0};
};

struct Result {
  std::string implementation;
  std::string operation;
  double qps{0.0};
};

double median(std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

std::vector<Result> run_implementation(const std::string& binary,
                                       std::string implementation,
                                       std::size_t operations,
                                       std::size_t pipeline) {
  const std::string suffix = std::to_string(::getpid()) + "-" + implementation;
  const std::string socket_path = "/tmp/goblin-list-work-" + suffix + ".sock";
  ServerProcess server(binary, implementation, socket_path);
  Client client(socket_path);

  constexpr std::size_t kFixtureValues = 100'000;
  const std::size_t warmups = std::max<std::size_t>(pipeline * 4, 4096);
  const std::vector<Workload> workloads{
      {"LLEN", {"LLEN", "bench"}, kFixtureValues, 0},
      {"LINDEX middle", {"LINDEX", "bench", "50000"}, kFixtureValues, 0},
      {"LRANGE 16", {"LRANGE", "bench", "49992", "50007"},
       kFixtureValues, 0},
      {"LSET middle", {"LSET", "bench", "50000", "replacement"},
       kFixtureValues, 0},
      {"LPUSH", {"LPUSH", "bench", "value"}, 0, 0},
      {"RPUSH", {"RPUSH", "bench", "value"}, 0, 0},
      {"LPOP", {"LPOP", "bench"}, 0, 1},
      {"RPOP", {"RPOP", "bench"}, 0, 1},
      {"LMOVE rotate", {"LMOVE", "bench", "bench", "RIGHT", "LEFT"},
       kFixtureValues, 0},
      {"RPOPLPUSH rotate", {"RPOPLPUSH", "bench", "bench"},
       kFixtureValues, 0},
      {"BLPOP ready", {"BLPOP", "missing", "bench", "0"}, 0, 1},
      {"BRPOP ready", {"BRPOP", "missing", "bench", "0"}, 0, 1},
      {"BLMOVE ready", {"BLMOVE", "bench", "bench", "RIGHT", "LEFT", "0"},
       kFixtureValues, 0},
      {"LMPOP COUNT 8",
       {"LMPOP", "2", "missing", "bench", "LEFT", "COUNT", "8"}, 0, 8},
      {"BLMPOP ready COUNT 8",
       {"BLMPOP", "0", "2", "missing", "bench", "LEFT", "COUNT", "8"},
       0, 8},
  };

  std::vector<Result> results;
  results.reserve(workloads.size());
  for (const auto& workload : workloads) {
    std::vector<double> samples;
    samples.reserve(3);
    for (int trial = 0; trial < 3; ++trial) {
      client.command(std::array<std::string_view, 2>{"DEL", "bench"});
      const auto required = workload.initial_values +
                            workload.values_per_operation *
                                (operations + warmups);
      if (required != 0) {
        client.populate("bench", required, pipeline);
      }
      client.repeated(workload.command, warmups, pipeline);
      const auto start = Clock::now();
      client.repeated(workload.command, operations, pipeline);
      const double seconds =
          std::chrono::duration<double>(Clock::now() - start).count();
      samples.push_back(static_cast<double>(operations) / seconds);
    }
    results.push_back(Result{.implementation = implementation,
                             .operation = workload.name,
                             .qps = median(std::move(samples))});
  }
  return results;
}

std::size_t parse_size(const char* text, std::string_view name) {
  char* end = nullptr;
  const auto value = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0' || value == 0) {
    throw std::runtime_error(std::string(name) + " must be a positive integer");
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || argc > 4) {
      std::cerr << "usage: list_work_queue_benchmark <goblin-core> "
                   "[operations=200000] [pipeline=64]\n";
      return 2;
    }
    const std::size_t operations =
        argc >= 3 ? parse_size(argv[2], "operations") : 200'000;
    const std::size_t pipeline =
        argc >= 4 ? parse_size(argv[3], "pipeline") : 64;

    auto segmented =
        run_implementation(argv[1], "segmented", operations, pipeline);
    auto pma = run_implementation(argv[1], "pma", operations, pipeline);

    std::cout << "implementation,operation,operations,pipeline,qps\n";
    std::cout << std::fixed << std::setprecision(2);
    for (const auto& result : segmented) {
      std::cout << result.implementation << ',' << result.operation << ','
                << operations << ',' << pipeline << ',' << result.qps << '\n';
    }
    for (const auto& result : pma) {
      std::cout << result.implementation << ',' << result.operation << ','
                << operations << ',' << pipeline << ',' << result.qps << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "list_work_queue_benchmark: " << error.what() << '\n';
    return 1;
  }
}
