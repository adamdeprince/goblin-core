#pragma once

#if defined(GOBLIN_HAS_AERON)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace goblin::core::aeron {

// Aeron channels are unidirectional. A Goblin connection therefore pairs one
// request channel/stream with one response-control channel/stream. The
// transport adds Aeron's response-channel correlation parameters internally.
struct ChannelConfig {
  std::string request_channel;
  std::int32_t request_stream_id{0};
  std::string response_channel;
  std::int32_t response_stream_id{0};

  // Endpoint arguments may be either bare HOST:PORT values or complete Aeron
  // UDP channel URIs. The response-control endpoint belongs to the server and
  // is passed unchanged by remote clients; response correlation supplies the
  // client's return destination. Complete URIs preserve optional settings.
  [[nodiscard]] static ChannelConfig udp(
      std::string_view request_endpoint_or_channel,
      std::int32_t request_stream_id,
      std::string_view response_control_endpoint_or_channel,
      std::int32_t response_stream_id);

  [[nodiscard]] static ChannelConfig ipc(std::int32_t request_stream_id,
                                         std::int32_t response_stream_id);
};

namespace detail {
struct ServerState;
struct PeerState;
struct ClientState;
}  // namespace detail

// One Aeron request image and its correlated response publication. Messages
// are reassembled before peek(), so the server sees the same record-oriented
// surface as its shared-memory, RDMA, and libfabric transports.
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
  [[nodiscard]] bool try_push(std::string_view bytes) noexcept;
  [[nodiscard]] std::size_t max_record_payload() const noexcept;
  void disconnect() noexcept;

 private:
  Connection(std::shared_ptr<detail::ServerState> server,
             std::shared_ptr<detail::PeerState> peer) noexcept;

  std::shared_ptr<detail::ServerState> server_;
  std::shared_ptr<detail::PeerState> peer_;

  friend class ServerListener;
};

struct ListenerPoll {
  bool progressed{false};
  std::unique_ptr<Connection> connection;
};

// One --aeron-* target owns an Aeron client connected to an external Media
// Driver. Incoming publications become independent Goblin connections.
class ServerListener {
 public:
  ServerListener(const ServerListener&) = delete;
  ServerListener& operator=(const ServerListener&) = delete;
  ServerListener(ServerListener&&) noexcept;
  ServerListener& operator=(ServerListener&&) noexcept;
  ~ServerListener();

  [[nodiscard]] static std::unique_ptr<ServerListener> create(
      const ChannelConfig& channels, std::string_view aeron_directory,
      std::string& error);

  [[nodiscard]] ListenerPoll poll() noexcept;
  [[nodiscard]] std::string_view error() const noexcept;

 private:
  explicit ServerListener(
      std::shared_ptr<detail::ServerState> state) noexcept;

  std::shared_ptr<detail::ServerState> state_;
};

// Compile-time adapter used by BasicSbeClient and by AeronClient below. A Media
// Driver must already be running on the local host; UDP peers each use their
// own local driver, while IPC peers use the same driver directory.
class ClientTransport {
 public:
  using ms = std::chrono::milliseconds;

  ClientTransport(const ClientTransport&) = delete;
  ClientTransport& operator=(const ClientTransport&) = delete;
  ClientTransport(ClientTransport&&) noexcept;
  ClientTransport& operator=(ClientTransport&&) noexcept;
  ~ClientTransport();

  [[nodiscard]] static std::optional<ClientTransport> open(
      const ChannelConfig& channels, ms timeout = ms(5000),
      std::size_t buffer_size = 64U * 1024U,
      std::string_view aeron_directory = {}, std::string* error = nullptr);

  [[nodiscard]] static std::optional<ClientTransport> open(
      std::string_view request_channel, std::int32_t request_stream_id,
      std::string_view response_channel, std::int32_t response_stream_id,
      ms timeout = ms(5000), std::size_t buffer_size = 64U * 1024U,
      std::string_view aeron_directory = {}, std::string* error = nullptr);

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    if (bytes.size() > max_message_bytes()) return false;
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
  std::size_t buffer_size_{0};
};

}  // namespace goblin::core::aeron

#endif  // GOBLIN_HAS_AERON
