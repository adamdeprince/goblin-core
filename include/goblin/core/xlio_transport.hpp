#pragma once

#if defined(GOBLIN_HAS_XLIO)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace goblin::core::xlio {

// True when the process was started with an XLIO library that exposes the
// pinned Ultra API. This does not initialize a polling group.
[[nodiscard]] bool runtime_available() noexcept;

namespace detail {
struct ConnectionState;
struct PollGroupState;
struct ListenerState;
}  // namespace detail

// One XLIO Ultra TCP stream. RX buffers remain owned by XLIO until pop(); TX
// uses Ultra's inline-copy path, which is appropriate for short RESP replies and
// does not require a per-connection registered-memory pool.
class Connection {
 public:
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept;
  Connection& operator=(Connection&&) noexcept;
  ~Connection();

  [[nodiscard]] bool established() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] bool terminated() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] std::string_view error() const noexcept;

  [[nodiscard]] std::optional<std::string_view> peek() noexcept;
  void pop() noexcept;
  [[nodiscard]] bool try_push(std::string_view bytes) noexcept;
  [[nodiscard]] std::size_t max_record_payload() const noexcept;
  void close() noexcept;

 private:
  explicit Connection(detail::ConnectionState* state) noexcept;

  detail::ConnectionState* state_{nullptr};

  friend class ServerListener;
  friend class ClientTransport;
};

struct ListenerPoll {
  bool progressed{false};
  std::unique_ptr<Connection> connection;
};

// One --xlio target owns one Ultra polling group. Keeping groups separate is
// deliberate: Server's outer target scan can preserve literal CLI priority.
class ServerListener {
 public:
  ServerListener(const ServerListener&) = delete;
  ServerListener& operator=(const ServerListener&) = delete;
  ServerListener(ServerListener&&) noexcept;
  ServerListener& operator=(ServerListener&&) noexcept;
  ~ServerListener();

  [[nodiscard]] static std::unique_ptr<ServerListener> create(
      std::string_view bind_address, std::uint16_t port, std::string& error);

  [[nodiscard]] ListenerPoll poll() noexcept;
  [[nodiscard]] std::string_view error() const noexcept;

 private:
  explicit ServerListener(
      std::unique_ptr<detail::ListenerState> state) noexcept;

  std::unique_ptr<detail::ListenerState> state_;
};

// Active Ultra connection used by command-line clients and benchmarks.
class ClientTransport {
 public:
  using ms = std::chrono::milliseconds;

  ClientTransport(const ClientTransport&) = delete;
  ClientTransport& operator=(const ClientTransport&) = delete;
  ClientTransport(ClientTransport&&) noexcept;
  ClientTransport& operator=(ClientTransport&&) noexcept;
  ~ClientTransport();

  [[nodiscard]] static std::optional<ClientTransport> open(
      std::string_view host, std::uint16_t port, ms timeout = ms(5000),
      std::string_view local_address = {}, std::string* error = nullptr);

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    while (!bytes.empty()) {
      if (connection_->try_push(bytes)) {
        return true;
      }
      if (connection_->failed() || connection_->closed() || stop()) {
        return false;
      }
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
  ClientTransport(std::shared_ptr<detail::PollGroupState> group,
                  std::unique_ptr<Connection> connection) noexcept;

  std::shared_ptr<detail::PollGroupState> group_;
  std::unique_ptr<Connection> connection_;
};

}  // namespace goblin::core::xlio

#endif  // GOBLIN_HAS_XLIO
