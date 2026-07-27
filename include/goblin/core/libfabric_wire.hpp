#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core::libfabric_wire {

inline constexpr std::array<char, 4> kMagic{'G', 'F', 'A', '1'};
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 40;
inline constexpr std::uint64_t kFirstSequence = 1;
inline constexpr std::size_t kDefaultReorderMessages = 64;
inline constexpr std::size_t kDefaultReorderBytes = 1024U * 1024U;

enum class Kind : std::uint8_t {
  hello = 1,
  hello_ack = 2,
  data = 3,
  ping = 4,
  pong = 5,
  goodbye = 6,
  error = 7,
};

struct Header {
  Kind kind{Kind::data};
  std::uint8_t flags{0};
  std::uint32_t payload_bytes{0};
  std::uint64_t session_id{0};
  std::uint64_t sequence{0};
  // A normal reply or PONG echoes the client request sequence. Pub/Sub pushes
  // and other unsolicited frames use zero.
  std::uint64_t reply_to{0};
};

namespace detail {

template <class T>
[[nodiscard]] constexpr T little_endian(T value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return value;
  } else {
    return std::byteswap(value);
  }
}

template <class T>
void put(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
  value = little_endian(value);
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <class T>
[[nodiscard]] T get(std::span<const std::byte> bytes,
                    std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return little_endian(value);
}

}  // namespace detail

[[nodiscard]] inline bool encode_header(std::span<std::byte> bytes,
                                        const Header& header) noexcept {
  if (bytes.size() < kHeaderBytes) return false;
  std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
  detail::put<std::uint16_t>(bytes, 4, kVersion);
  bytes[6] = static_cast<std::byte>(header.kind);
  bytes[7] = static_cast<std::byte>(header.flags);
  detail::put<std::uint32_t>(bytes, 8, header.payload_bytes);
  detail::put<std::uint32_t>(bytes, 12, 0);
  detail::put<std::uint64_t>(bytes, 16, header.session_id);
  detail::put<std::uint64_t>(bytes, 24, header.sequence);
  detail::put<std::uint64_t>(bytes, 32, header.reply_to);
  return true;
}

[[nodiscard]] inline std::optional<Header> decode_header(
    std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < kHeaderBytes ||
      std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0 ||
      detail::get<std::uint16_t>(bytes, 4) != kVersion) {
    return std::nullopt;
  }
  const auto kind = static_cast<Kind>(std::to_integer<std::uint8_t>(bytes[6]));
  if (kind < Kind::hello || kind > Kind::error) return std::nullopt;

  Header result{
      .kind = kind,
      .flags = std::to_integer<std::uint8_t>(bytes[7]),
      .payload_bytes = detail::get<std::uint32_t>(bytes, 8),
      .session_id = detail::get<std::uint64_t>(bytes, 16),
      .sequence = detail::get<std::uint64_t>(bytes, 24),
      .reply_to = detail::get<std::uint64_t>(bytes, 32),
  };
  if (result.payload_bytes != bytes.size() - kHeaderBytes) {
    return std::nullopt;
  }
  return result;
}

struct HelloView {
  std::uint32_t reorder_messages{0};
  std::string_view goblin_version;
  std::span<const std::byte> endpoint_address;
};

[[nodiscard]] inline std::size_t hello_payload_bytes(
    std::string_view goblin_version, std::size_t address_bytes) noexcept {
  return 8 + goblin_version.size() + address_bytes;
}

[[nodiscard]] inline bool encode_hello(
    std::span<std::byte> bytes, std::string_view goblin_version,
    std::span<const std::byte> endpoint_address,
    std::uint32_t reorder_messages) noexcept {
  if (goblin_version.size() > std::numeric_limits<std::uint16_t>::max() ||
      endpoint_address.size() > std::numeric_limits<std::uint16_t>::max() ||
      bytes.size() !=
          hello_payload_bytes(goblin_version, endpoint_address.size())) {
    return false;
  }
  detail::put<std::uint16_t>(
      bytes, 0, static_cast<std::uint16_t>(goblin_version.size()));
  detail::put<std::uint16_t>(
      bytes, 2, static_cast<std::uint16_t>(endpoint_address.size()));
  detail::put<std::uint32_t>(bytes, 4, reorder_messages);
  std::memcpy(bytes.data() + 8, goblin_version.data(), goblin_version.size());
  std::memcpy(bytes.data() + 8 + goblin_version.size(),
              endpoint_address.data(), endpoint_address.size());
  return true;
}

[[nodiscard]] inline std::optional<HelloView> decode_hello(
    std::span<const std::byte> bytes) noexcept {
  if (bytes.size() < 8) return std::nullopt;
  const auto version_bytes = detail::get<std::uint16_t>(bytes, 0);
  const auto address_bytes = detail::get<std::uint16_t>(bytes, 2);
  if (8U + version_bytes + address_bytes != bytes.size()) {
    return std::nullopt;
  }
  return HelloView{
      .reorder_messages = detail::get<std::uint32_t>(bytes, 4),
      .goblin_version = std::string_view(
          reinterpret_cast<const char*>(bytes.data() + 8), version_bytes),
      .endpoint_address =
          bytes.subspan(8U + version_bytes, address_bytes),
  };
}

struct HelloAck {
  std::uint32_t heartbeat_timeout_ms{0};
  std::uint32_t reorder_messages{0};
  std::uint32_t max_payload_bytes{0};
};

inline constexpr std::size_t kHelloAckBytes = 12;

[[nodiscard]] inline bool encode_hello_ack(
    std::span<std::byte> bytes, const HelloAck& ack) noexcept {
  if (bytes.size() != kHelloAckBytes) return false;
  detail::put<std::uint32_t>(bytes, 0, ack.heartbeat_timeout_ms);
  detail::put<std::uint32_t>(bytes, 4, ack.reorder_messages);
  detail::put<std::uint32_t>(bytes, 8, ack.max_payload_bytes);
  return true;
}

[[nodiscard]] inline std::optional<HelloAck> decode_hello_ack(
    std::span<const std::byte> bytes) noexcept {
  if (bytes.size() != kHelloAckBytes) return std::nullopt;
  return HelloAck{
      .heartbeat_timeout_ms = detail::get<std::uint32_t>(bytes, 0),
      .reorder_messages = detail::get<std::uint32_t>(bytes, 4),
      .max_payload_bytes = detail::get<std::uint32_t>(bytes, 8),
  };
}

enum class OrderResult {
  delivered,
  sequestered,
  duplicate,
  overflow,
};

// The common path owns no reorder storage: FI_ORDER_SAS delivers immediately.
// Only an observed gap allocates. The bounded vector avoids a permanent table
// per client while preventing a broken peer from retaining unlimited payloads.
template <class Message>
class OrderedInbox {
 public:
  explicit OrderedInbox(
      std::size_t max_messages = kDefaultReorderMessages,
      std::size_t max_bytes = kDefaultReorderBytes) noexcept
      : max_messages_(max_messages), max_bytes_(max_bytes) {}

  [[nodiscard]] std::uint64_t next_expected() const noexcept {
    return next_expected_;
  }
  [[nodiscard]] std::size_t sequestered_messages() const noexcept {
    return pending_.size();
  }
  [[nodiscard]] std::size_t sequestered_bytes() const noexcept {
    return pending_bytes_;
  }

  template <class Deliver>
  OrderResult push(std::uint64_t sequence, Message message,
                   std::size_t message_bytes, Deliver&& deliver) {
    if (sequence < next_expected_) return OrderResult::duplicate;
    if (sequence == next_expected_) {
      deliver(std::move(message), sequence);
      ++next_expected_;
      drain(deliver);
      return OrderResult::delivered;
    }

    if (sequence - next_expected_ >= max_messages_ ||
        pending_.size() >= max_messages_ ||
        message_bytes > max_bytes_ - pending_bytes_) {
      return OrderResult::overflow;
    }
    for (const auto& item : pending_) {
      if (item.sequence == sequence) return OrderResult::duplicate;
    }
    pending_bytes_ += message_bytes;
    pending_.push_back(Pending{sequence, message_bytes, std::move(message)});
    return OrderResult::sequestered;
  }

  void reset() noexcept {
    next_expected_ = kFirstSequence;
    pending_.clear();
    pending_bytes_ = 0;
  }

 private:
  struct Pending {
    std::uint64_t sequence;
    std::size_t bytes;
    Message message;
  };

  template <class Deliver>
  void drain(Deliver&& deliver) {
    for (;;) {
      auto found = pending_.end();
      for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->sequence == next_expected_) {
          found = it;
          break;
        }
      }
      if (found == pending_.end()) return;

      Pending ready = std::move(*found);
      pending_bytes_ -= ready.bytes;
      *found = std::move(pending_.back());
      pending_.pop_back();
      deliver(std::move(ready.message), ready.sequence);
      ++next_expected_;
    }
  }

  std::uint64_t next_expected_{kFirstSequence};
  std::vector<Pending> pending_;
  std::size_t pending_bytes_{0};
  std::size_t max_messages_;
  std::size_t max_bytes_;
};

}  // namespace goblin::core::libfabric_wire
