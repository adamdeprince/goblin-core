#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace goblin::core {

class Store;

// One shared-memory ring buffer, from `--ring <path> <bytes>`. `bytes` is the
// requested capacity per direction; it is rounded up to a power-of-two page
// multiple when the ring is created (see ring_buffer.hpp).
struct RingConfig {
  std::string path;
  std::uint64_t bytes{0};
};

struct ServerConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{6379};
  // When non-empty, listen on this AF_UNIX path instead of TCP -- no network
  // stack, so the per-round-trip cost of a synchronous client drops sharply.
  std::string unix_socket_path{};
  int backlog{128};
  std::size_t max_output_buffer_bytes{1024U * 1024U};
  std::size_t resume_output_buffer_bytes{256U * 1024U};
  std::size_t initial_output_buffer_bytes{0};
  // Per-connection socket read buffer (the chunk each recv() fills). Configurable so
  // an operator can trade memory for fewer syscalls on large-message workloads.
  std::size_t client_read_buffer_bytes{16U * 1024U};
  // Shared-memory rings, highest priority first. When non-empty the server
  // busy-polls these before touching the network (and spins at 100% CPU by
  // design); when empty it runs the ordinary low-CPU poll() loop.
  std::vector<RingConfig> rings{};
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
