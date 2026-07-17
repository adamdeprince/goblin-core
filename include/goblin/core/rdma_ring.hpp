#pragma once

#if defined(GOBLIN_HAS_RDMA)

#include "goblin/core/ring_buffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace goblin::core::rdma {

// The remote write is variable-length, but every slot's commit word is fixed at
// the end of a 256-byte stride. The payload is right-aligned immediately before
// it, so a two-byte message puts only ten bytes on the wire. The whole hot-path
// write remains below Connect-IB's 220-byte inline limit.
inline constexpr std::size_t kSlotStride = 256;
inline constexpr std::size_t kCommitOffset = kSlotStride - sizeof(std::uint64_t);
inline constexpr std::size_t kMaxPayload = 192;

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

  // Receiver side: inspect the next published fragment in the local registered
  // ring, then release its slot with pop(). The view remains valid until pop().
  [[nodiscard]] std::optional<std::string_view> peek() noexcept;
  void pop() noexcept;
  void wait_for_record() const noexcept { ring::cpu_relax(); }

  // Sender side: one inline RC RDMA WRITE containing payload followed by the
  // 56-bit-sequence/8-bit-length commit. try_push may synchronously refresh
  // remote credits only when its cached view says the ring is full.
  [[nodiscard]] bool try_push(std::string_view payload) noexcept;
  [[nodiscard]] std::size_t max_record_payload() const noexcept {
    return kMaxPayload;
  }

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    while (!bytes.empty()) {
      const std::size_t length =
          bytes.size() < kMaxPayload ? bytes.size() : kMaxPayload;
      const std::string_view fragment = bytes.substr(0, length);
      while (!try_push(fragment)) {
        if (failed() || disconnected() || stop()) {
          return false;
        }
        ring::cpu_relax();
      }
      bytes.remove_prefix(length);
    }
    return true;
  }

  [[nodiscard]] std::size_t inbound_capacity() const noexcept;
  [[nodiscard]] std::size_t outbound_capacity() const noexcept;
  void disconnect() noexcept;

 private:
  struct Impl;
  explicit Connection(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;

  friend class ServerListener;
  friend class ClientTransport;
};

struct ListenerPoll {
  bool progressed{false};
  std::unique_ptr<Connection> connection;
};

// One RDMA-CM listener is one command-line poll target. Its accepted connections
// share that target's priority but each owns an RC QP and two one-sided rings.
class ServerListener {
 public:
  ServerListener(const ServerListener&) = delete;
  ServerListener& operator=(const ServerListener&) = delete;
  ServerListener(ServerListener&&) noexcept;
  ServerListener& operator=(ServerListener&&) noexcept;
  ~ServerListener();

  [[nodiscard]] static std::unique_ptr<ServerListener> create(
      std::string_view address, std::uint16_t port, std::uint64_t ring_bytes,
      int backlog, int numa_node, std::string& error);

  // Non-blocking. Connection-request events return the newly accepted object;
  // establishment and disconnect events update the existing object's state.
  [[nodiscard]] ListenerPoll poll() noexcept;
  [[nodiscard]] std::string_view error() const noexcept;

 private:
  struct Impl;
  explicit ServerListener(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

// Compile-time transport adapter used by the typed SBE client. It intentionally
// mirrors the tiny Producer/Consumer surface of the shared-memory ring without a
// virtual call on the hot path.
class ClientTransport {
 public:
  using ms = std::chrono::milliseconds;

  ClientTransport(const ClientTransport&) = delete;
  ClientTransport& operator=(const ClientTransport&) = delete;
  ClientTransport(ClientTransport&&) noexcept = default;
  ClientTransport& operator=(ClientTransport&&) noexcept = default;

  [[nodiscard]] static std::optional<ClientTransport> open(
      std::string_view address, std::uint16_t port, std::uint64_t ring_bytes,
      ms timeout = ms(5000), std::size_t buffer_size = 16 * 1024,
      std::string* error = nullptr);

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    return connection_->send(bytes, std::forward<StopFn>(stop));
  }
  [[nodiscard]] std::optional<std::string_view> peek() noexcept {
    return connection_->peek();
  }
  void pop() noexcept { connection_->pop(); }
  void wait_for_record() const noexcept { connection_->wait_for_record(); }
  [[nodiscard]] std::size_t send_capacity() const noexcept {
    return connection_->outbound_capacity();
  }
  [[nodiscard]] std::size_t receive_capacity() const noexcept {
    return connection_->inbound_capacity();
  }
  [[nodiscard]] std::size_t max_message_bytes() const noexcept {
    return connection_->outbound_capacity();
  }
  [[nodiscard]] std::size_t buffer_size_hint() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] Connection& connection() noexcept { return *connection_; }

 private:
  ClientTransport(std::unique_ptr<Connection> connection,
                  std::size_t buffer_size) noexcept
      : connection_(std::move(connection)), buffer_size_(buffer_size) {}

  std::unique_ptr<Connection> connection_;
  std::size_t buffer_size_{0};
};

}  // namespace goblin::core::rdma

#endif  // GOBLIN_HAS_RDMA
