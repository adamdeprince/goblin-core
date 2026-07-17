#pragma once

#if defined(GOBLIN_HAS_RDMA)

#include "goblin/core/rdma_ring.hpp"
#include "goblin/core/ring_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace goblin::core::rdma {

// RESP2/RESP3 client over the bidirectional one-sided RDMA rings. Commands and
// replies retain their ordinary RESP byte stream; only the transport changes.
class RdmaClient {
 public:
  using ms = std::chrono::milliseconds;
  static constexpr ms kDefaultTimeout{5000};

  [[nodiscard]] static std::optional<RdmaClient> open(
      std::string_view address, std::uint16_t port, std::uint64_t ring_bytes,
      ms timeout = kDefaultTimeout, std::string* error = nullptr) {
    auto transport = ClientTransport::open(address, port, ring_bytes, timeout,
                                           16 * 1024, error);
    if (!transport) {
      return std::nullopt;
    }
    return RdmaClient(std::move(*transport));
  }

  void send_raw(std::string_view bytes, ms timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!transport_.send(bytes, [&] {
          return std::chrono::steady_clock::now() >= deadline;
        })) {
      std::string message = "RdmaClient: request send failed";
      if (!transport_.connection().error().empty()) {
        message += ": ";
        message += transport_.connection().error();
      }
      throw std::runtime_error(message);
    }
  }

  void send(std::span<const std::string_view> args,
            ms timeout = kDefaultTimeout) {
    send_raw(ring::encode_command(args), timeout);
  }

  [[nodiscard]] std::optional<std::string> read_reply(
      ms timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    unsigned spins = 0;
    for (;;) {
      if (const auto end = ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      if (auto record = transport_.peek()) {
        pending_.append(*record);
        transport_.pop();
        continue;
      }
      if (transport_.connection().failed() ||
          transport_.connection().disconnected()) {
        return std::nullopt;
      }
      if ((++spins & 63U) == 0 &&
          std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      transport_.wait_for_record();
    }
  }

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> args,
      ms timeout = kDefaultTimeout) {
    send(args, timeout);
    return read_reply(timeout);
  }

  [[nodiscard]] std::optional<std::string> command(
      std::initializer_list<std::string_view> args,
      ms timeout = kDefaultTimeout) {
    return command(
        std::span<const std::string_view>(args.begin(), args.size()), timeout);
  }

  // Pipeline one command while servicing already-produced replies whenever the
  // outbound ring is full. Without this, a writer that fills both directions
  // could wait for request credits while the server waits for reply credits.
  void send_pipelined(std::span<const std::string_view> args,
                      ms timeout = kDefaultTimeout) {
    const std::string bytes = ring::encode_command(args);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool sent = transport_.send(bytes, [&] {
      (void)drain();
      return std::chrono::steady_clock::now() >= deadline;
    });
    if (!sent) {
      throw std::runtime_error(
          "RdmaClient: timed out streaming a pipelined command");
    }
  }

  std::size_t drain() {
    std::size_t bytes = 0;
    while (auto record = transport_.peek()) {
      pending_.append(*record);
      bytes += record->size();
      transport_.pop();
    }
    return bytes;
  }

  [[nodiscard]] std::optional<std::string> try_read_reply() {
    for (;;) {
      if (const auto end = ring::reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      auto record = transport_.peek();
      if (!record) {
        return std::nullopt;
      }
      pending_.append(*record);
      transport_.pop();
    }
  }

  [[nodiscard]] ClientTransport& transport() noexcept { return transport_; }

 private:
  explicit RdmaClient(ClientTransport&& transport) noexcept
      : transport_(std::move(transport)) {}

  ClientTransport transport_;
  std::string pending_;
};

}  // namespace goblin::core::rdma

#endif  // GOBLIN_HAS_RDMA
