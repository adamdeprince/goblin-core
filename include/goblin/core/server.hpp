#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace goblin::core {

class Store;

struct ServerConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{6379};
  int backlog{128};
  std::size_t max_output_buffer_bytes{1024U * 1024U};
  std::size_t resume_output_buffer_bytes{256U * 1024U};
  std::size_t initial_output_buffer_bytes{0};
};

class Server {
 public:
  Server(ServerConfig config, Store& store);

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  int run();
  void stop() noexcept;

 private:
  ServerConfig config_;
  Store& store_;
  std::atomic_bool running_{false};
};

}  // namespace goblin::core
