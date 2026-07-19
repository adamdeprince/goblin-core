#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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

// One InfiniBand/RDMA listener from `--rdma <address> <port> <bytes>`. Each
// accepted peer gets a registered inbound ring of approximately `bytes`; the
// reverse direction is provided by the peer's registered ring.
struct RdmaConfig {
  std::string bind_address;
  std::uint16_t port{0};
  std::uint64_t bytes{0};
};

// One ExaSock-priority TCP listener from `--exasock <address> <port>`. Serviced
// in the same strict busy-poll order as rings and RDMA (before the sparse plain
// socket pass). Under the `exasock` LD_PRELOAD wrapper and an ExaNIC bind
// address, accepted sockets are kernel-bypassed; without the wrapper this is
// still a high-priority TCP target.
struct ExasockConfig {
  std::string bind_address;
  std::uint16_t port{0};
};

// Polled targets retain their literal command-line order. A mixed sequence such
// as `--ring A --exasock B --rdma C --ring D` is therefore scanned A, B, C, D,
// followed by the sparse plain-socket pass.
using PollTargetConfig =
    std::variant<RingConfig, RdmaConfig, ExasockConfig>;

// One outbound SBE Pub/Sub subscription. The local server subscribes to the
// upstream Goblin Core instance and republishes received channel/payload pairs
// through its own PubSubRegistry.
struct PubSubListenerRingConfig {
  std::string path;
};

struct PubSubListenerRdmaConfig {
  std::string address;
  std::uint16_t port{0};
  std::uint64_t bytes{0};
};

struct PubSubListenerUdsConfig {
  std::string path;
};

struct PubSubListenerTcpConfig {
  std::string address;
  std::uint16_t port{0};
};

using PubSubListenerConfig =
    std::variant<PubSubListenerRingConfig, PubSubListenerRdmaConfig,
                 PubSubListenerUdsConfig, PubSubListenerTcpConfig>;

// Ordinary RESP/SBE socket listeners. These are intentionally separate from
// polled ring, RDMA, and ExaSock targets: every configured socket participates
// in the same sparse poll() pass.
struct TcpListenerConfig {
  std::string bind_address;
  std::uint16_t port{0};
};

struct UdsListenerConfig {
  std::string path;
};

using SocketListenerConfig =
    std::variant<TcpListenerConfig, UdsListenerConfig>;

// One Kafka topic carrying exactly one RESP2 command array per record. A
// timestamp is present when startup also loaded a snapshot; otherwise every
// retained record is replayed.
struct KafkaConfig {
  std::string connection;
  std::optional<std::int64_t> start_timestamp_ms;
};

struct ServerConfig {
  // Legacy single-listener fields used by --bind/--port/--unixsocket and by
  // callers constructing ServerConfig directly. socket_listeners takes
  // precedence when it is non-empty.
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{6379};
  std::string unix_socket_path{};
  // Repeatable --tcp-listen and --uds-listen endpoints. Multiple TCP and UDS
  // listeners can coexist and all serve the same Store.
  std::vector<SocketListenerConfig> socket_listeners{};
  int backlog{128};
  std::size_t max_output_buffer_bytes{1024U * 1024U};
  std::size_t resume_output_buffer_bytes{256U * 1024U};
  std::size_t initial_output_buffer_bytes{0};
  // Per-client anonymous mapping used only for unsolicited Pub/Sub delivery.
  // Zero selects one native page; non-zero values round up to a whole-page mapping.
  std::size_t unsolicited_output_buffer_bytes{0};
  // Fixed anonymous mapping that holds queued MULTI commands. Zero selects one
  // native page; non-zero values round up to a whole-page mapping.
  std::size_t transaction_buffer_bytes{0};
  // Per-connection socket read buffer (the chunk each recv() fills). Configurable so
  // an operator can trade memory for fewer syscalls on large-message workloads.
  std::size_t client_read_buffer_bytes{16U * 1024U};
  // Shared-memory rings, ExaSock TCP listeners, and RDMA targets, highest priority
  // first (literal CLI order). When non-empty the server busy-polls these before
  // the sparse plain-socket pass (and spins at 100% CPU by design); when empty it
  // runs the ordinary low-CPU poll() loop.
  std::vector<PollTargetConfig> poll_targets{};
  // Optional upstream Goblin Core instance. It is always an SBE PSUBSCRIBE
  // client and therefore requires the exact same Goblin Core version.
  std::optional<PubSubListenerConfig> pubsub_listener{};
  std::string pubsub_listener_pattern{"*"};
  std::optional<KafkaConfig> kafka{};
  // Optional RESP credential database created by goblin-core-auth. When set,
  // TCP and UDS connections authenticate before accessing data commands.
  std::optional<std::string> auth_file{};
  // SBE is a trusted-fabric protocol and intentionally has no AUTH exchange.
  // It is disabled unless explicitly requested at startup.
  bool enable_sbe{false};
  // RESP still authenticates on rings/RDMA by default. These switches mark a
  // whole transport class as inside the operator's trusted boundary.
  bool no_auth_ring{false};
  bool no_auth_rdma{false};
  // Back the rings with huge pages (Linux hugetlbfs) to cut ring TLB pressure. The
  // requested size rounds up to the huge-page size, and each --ring PATH becomes a
  // symlink into the hugetlbfs mount. Linux-only; rejected at startup elsewhere.
  bool ring_hugetlb{false};
  // Pin the server to CPU `cpu` (>= 0) and place the ring memory on that CPU's NUMA
  // node -- strictly, so a ring that cannot be made node-local is a fatal startup
  // error (a remote ring wrecks the latency it exists for). -1 disables it.
  int cpu{-1};
  // Resolved slice for CPU affinity, shared rings, and server-side registered RDMA
  // rings. It may come from --numa, --cpu, or unanimous endpoint auto-discovery.
  int numa_node{-1};
  // Additionally steer arena allocations to the selected slice (soft, best-effort).
  // Off by default because pinning all of a large server's memory to one node can
  // starve clients co-located on it.
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
