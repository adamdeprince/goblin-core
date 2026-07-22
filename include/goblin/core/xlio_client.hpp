#pragma once

#if defined(GOBLIN_HAS_XLIO)

#include "goblin/core/ring_client.hpp"
#include "goblin/core/xlio_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace goblin::core::xlio {

class XlioClient {
 public:
  using ms = std::chrono::milliseconds;
  static constexpr ms kDefaultTimeout{5000};

  [[nodiscard]] static std::optional<XlioClient> open(
      std::string_view host, std::uint16_t port,
      ms timeout = kDefaultTimeout, std::string_view local_address = {},
      std::string* error = nullptr) {
    auto transport =
        ClientTransport::open(host, port, timeout, local_address, error);
    if (!transport) return std::nullopt;
    return XlioClient(std::move(*transport));
  }

  void send_raw(std::string_view bytes, ms timeout = kDefaultTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!transport_.send(bytes, [&] {
          return std::chrono::steady_clock::now() >= deadline;
        })) {
      throw std::runtime_error("XlioClient: request send failed");
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
      if (auto fragment = transport_.peek()) {
        pending_.append(*fragment);
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
    send(args, timeout);
  }

  std::size_t drain() {
    std::size_t bytes = 0;
    while (auto fragment = transport_.peek()) {
      pending_.append(*fragment);
      bytes += fragment->size();
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
      auto fragment = transport_.peek();
      if (!fragment) return std::nullopt;
      pending_.append(*fragment);
      transport_.pop();
    }
  }

  [[nodiscard]] ClientTransport& transport() noexcept { return transport_; }
  [[nodiscard]] const ClientTransport& transport() const noexcept {
    return transport_;
  }

 private:
  explicit XlioClient(ClientTransport&& transport) noexcept
      : transport_(std::move(transport)) {}

  ClientTransport transport_;
  std::string pending_;
};

}  // namespace goblin::core::xlio

#endif  // GOBLIN_HAS_XLIO
