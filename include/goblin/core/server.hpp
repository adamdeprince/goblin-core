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
  // Per-client anonymous mapping used only for unsolicited Pub/Sub delivery.
  // Zero selects one native page; non-zero values round up to a whole-page mapping.
  std::size_t unsolicited_output_buffer_bytes{0};
  // Per-connection socket read buffer (the chunk each recv() fills). Configurable so
  // an operator can trade memory for fewer syscalls on large-message workloads.
  std::size_t client_read_buffer_bytes{16U * 1024U};
  // Shared-memory rings, highest priority first. When non-empty the server
  // busy-polls these before touching the network (and spins at 100% CPU by
  // design); when empty it runs the ordinary low-CPU poll() loop.
  std::vector<RingConfig> rings{};
  // Back the rings with huge pages (Linux hugetlbfs) to cut ring TLB pressure. The
  // requested size rounds up to the huge-page size, and each --ring PATH becomes a
  // symlink into the hugetlbfs mount. Linux-only; rejected at startup elsewhere.
  bool ring_hugetlb{false};
  // Pin the server to CPU `cpu` (>= 0) and place the ring memory on that CPU's NUMA
  // node -- strictly, so a ring that cannot be made node-local is a fatal startup
  // error (a remote ring wrecks the latency it exists for). -1 disables it.
  int cpu{-1};
  // Additionally steer the arenas' allocations to the pinned CPU's node (soft,
  // best-effort). Only meaningful with `cpu` set; off by default because pinning all
  // of a large server's memory to one node can starve clients co-located on it.
  bool numa_arena{false};
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
