#pragma once

#if defined(GOBLIN_HAS_AERON)

#include "goblin/core/aeron_transport.hpp"
#include "goblin/core/ring_client.hpp"

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace goblin::core::aeron {

// RESP2/RESP3 over an Aeron request/response channel pair. The typed SBE client
// is exposed as goblin::core::SbeAeronClient from sbe_ring_client.hpp.
class AeronClient {
 public:
  using ms = std::chrono::milliseconds;
  static constexpr ms kDefaultTimeout{5000};

  [[nodiscard]] static std::optional<AeronClient> open(
      const ChannelConfig& channels, ms timeout = kDefaultTimeout,
      std::string_view aeron_directory = {}, std::string* error = nullptr) {
    auto transport = ClientTransport::open(
        channels, timeout, 64U * 1024U, aeron_directory, error);
    if (!transport) return std::nullopt;
    return AeronClient(std::move(*transport));
  }

  void send_raw(std::string_view bytes, ms timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!transport_.send(bytes, [&] {
          return std::chrono::steady_clock::now() >= deadline;
        })) {
      std::string message = "AeronClient: request send failed";
      if (!transport_.error().empty()) {
        message.append(": ");
        message.append(transport_.error());
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
      if (transport_.failed()) return std::nullopt;
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
          "AeronClient: timed out sending a pipelined command");
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
      if (!record) return std::nullopt;
      pending_.append(*record);
      transport_.pop();
    }
  }

  [[nodiscard]] ClientTransport& transport() noexcept { return transport_; }
  [[nodiscard]] const ClientTransport& transport() const noexcept {
    return transport_;
  }

 private:
  explicit AeronClient(ClientTransport&& transport) noexcept
      : transport_(std::move(transport)) {}

  ClientTransport transport_;
  std::string pending_;
};

}  // namespace goblin::core::aeron

#endif  // GOBLIN_HAS_AERON
