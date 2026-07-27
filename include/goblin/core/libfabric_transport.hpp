#pragma once

#if defined(GOBLIN_HAS_LIBFABRIC)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace goblin::core::fabric {

inline constexpr std::uint32_t kDefaultHeartbeatTimeoutMs = 3000;
inline constexpr std::size_t kDefaultReorderMessages = 64;
inline constexpr std::size_t kDefaultReorderBytes = 1024U * 1024U;

enum class SendMode : std::uint8_t {
  auto_inject,
  send,
};

namespace detail {
struct ServerState;
struct PeerState;
struct ClientState;
}  // namespace detail

class Connection {
 public:
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  ~Connection();

  [[nodiscard]] bool established() const noexcept;
  [[nodiscard]] bool disconnected() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] std::string_view error() const noexcept;

  [[nodiscard]] std::optional<std::string_view> peek() noexcept;
  void pop() noexcept;
  void wait_for_record() const noexcept;
  [[nodiscard]] std::uint64_t current_sequence() const noexcept;

  // The server sets this before publishing a normal response. Zero identifies
  // an unsolicited Pub/Sub or replication frame.
  void set_reply_to(std::uint64_t request_sequence) noexcept;
  [[nodiscard]] bool try_push(std::string_view bytes) noexcept;
  [[nodiscard]] std::size_t max_record_payload() const noexcept;
  void disconnect() noexcept;

 private:
  Connection(std::shared_ptr<detail::ServerState> server,
             std::shared_ptr<detail::PeerState> peer) noexcept;

  std::shared_ptr<detail::ServerState> server_;
  std::shared_ptr<detail::PeerState> peer_;
  std::uint64_t reply_to_{0};

  friend class ServerListener;
  friend struct detail::ServerState;
};

struct ListenerPoll {
  bool progressed{false};
  std::unique_ptr<Connection> connection;
};

// One FI_EP_RDM endpoint is one strict-priority server poll target. It owns one
// address vector, completion queues, and shared registered I/O pool, then
// multiplexes any number of logical Goblin clients by session id.
class ServerListener {
 public:
  ServerListener(const ServerListener&) = delete;
  ServerListener& operator=(const ServerListener&) = delete;
  ServerListener(ServerListener&&) noexcept;
  ServerListener& operator=(ServerListener&&) noexcept;
  ~ServerListener();

  [[nodiscard]] static std::unique_ptr<ServerListener> create(
      std::string_view provider, std::string_view bootstrap_address,
      std::uint16_t bootstrap_port, std::uint32_t heartbeat_timeout_ms,
      std::size_t reorder_messages, std::size_t reorder_bytes,
      std::string& error, SendMode send_mode = SendMode::auto_inject);

  [[nodiscard]] ListenerPoll poll() noexcept;
  [[nodiscard]] std::string_view error() const noexcept;
  [[nodiscard]] std::string_view provider() const noexcept;

 private:
  explicit ServerListener(
      std::shared_ptr<detail::ServerState> state) noexcept;

  std::shared_ptr<detail::ServerState> state_;
};

// Compile-time transport adapter for BasicSbeClient. The provider is usually
// "efa" in production and "verbs;ofi_rxm" or "tcp" for local qualification.
// A background heartbeat thread emits transport PINGs only while application
// traffic is idle. PONGs are consumed below the SBE layer.
class ClientTransport {
 public:
  using ms = std::chrono::milliseconds;

  ClientTransport(const ClientTransport&) = delete;
  ClientTransport& operator=(const ClientTransport&) = delete;
  ClientTransport(ClientTransport&&) noexcept;
  ClientTransport& operator=(ClientTransport&&) noexcept;
  ~ClientTransport();

  [[nodiscard]] static std::optional<ClientTransport> open(
      std::string_view provider, std::string_view host,
      std::uint16_t bootstrap_port, ms timeout = ms(5000),
      std::size_t buffer_size = 128U * 1024U,
      std::string_view local_address = {}, std::string* error = nullptr,
      SendMode send_mode = SendMode::auto_inject);

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    while (!send_one(bytes)) {
      if (failed() || stop()) return false;
      poll();
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string_view> peek() noexcept;
  void pop() noexcept;
  void wait_for_record() noexcept;
  void poll() noexcept;

  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] std::string_view error() const noexcept;
  [[nodiscard]] std::size_t send_capacity() const noexcept;
  [[nodiscard]] std::size_t receive_capacity() const noexcept;
  [[nodiscard]] std::size_t max_message_bytes() const noexcept;
  [[nodiscard]] std::size_t buffer_size_hint() const noexcept;

 private:
  explicit ClientTransport(std::shared_ptr<detail::ClientState> state,
                           std::size_t buffer_size) noexcept;
  [[nodiscard]] bool send_one(std::string_view bytes) noexcept;

  std::shared_ptr<detail::ClientState> state_;
  std::optional<std::string> current_record_;
  std::size_t buffer_size_{0};
};

}  // namespace goblin::core::fabric

#endif  // GOBLIN_HAS_LIBFABRIC
