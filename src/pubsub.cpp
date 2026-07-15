#include "pubsub.hpp"

#include "goblin/core/resp_writer.hpp"
#ifdef GOBLIN_HAS_SBE
#include "goblin/core/sbe_frame.hpp"
#include "goblin_sbe/ArrayReply.h"
#include "goblin_sbe/ErrorReply.h"
#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/PubSubNumSubReply.h"
#include "goblin_sbe/PubSubPush.h"
#include "goblin_sbe/RespValueReply.h"
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace goblin::core::detail {
namespace {

// Wire-format prefixes for literal and pattern deliveries. RESP2 uses '*'; RESP3
// uses '>' — same length, so dual-mode publish can encode once and flip byte 0.
inline constexpr std::string_view kResp2MessagePrefix{"*3\r\n$7\r\nmessage\r\n"};
inline constexpr std::string_view kResp3MessagePrefix{">3\r\n$7\r\nmessage\r\n"};
inline constexpr std::string_view kResp2PmessagePrefix{"*4\r\n$8\r\npmessage\r\n"};
inline constexpr std::string_view kResp3PmessagePrefix{">4\r\n$8\r\npmessage\r\n"};

constexpr std::uint8_t kModeResp2 = 1;
constexpr std::uint8_t kModeResp3 = 2;
constexpr std::uint8_t kModeSbe = 4;

[[nodiscard]] bool equals_ci(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const char a = lhs[i] >= 'A' && lhs[i] <= 'Z'
                       ? static_cast<char>(lhs[i] + ('a' - 'A'))
                       : lhs[i];
    const char b = rhs[i] >= 'A' && rhs[i] <= 'Z'
                       ? static_cast<char>(rhs[i] + ('a' - 'A'))
                       : rhs[i];
    if (a != b) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::size_t bulk_wire_size(std::size_t length) noexcept {
  return resp::detail::bulk_string_wire_size(length);
}

void append_bulk(std::string& out, std::string_view value) {
  resp::append_bulk_string(out, value);
}

void append_literal_body(std::string& out, std::string_view prefix,
                         std::string_view channel, std::string_view payload) {
  const std::size_t need = prefix.size() + bulk_wire_size(channel.size()) +
                           bulk_wire_size(payload.size());
  resp::reserve_append_capacity(out, need);
  out.append(prefix);
  append_bulk(out, channel);
  append_bulk(out, payload);
}

void append_pattern_body(std::string& out, std::string_view prefix,
                         std::string_view pattern, std::string_view channel,
                         std::string_view payload) {
  const std::size_t need = prefix.size() + bulk_wire_size(pattern.size()) +
                           bulk_wire_size(channel.size()) +
                           bulk_wire_size(payload.size());
  resp::reserve_append_capacity(out, need);
  out.append(prefix);
  append_bulk(out, pattern);
  append_bulk(out, channel);
  append_bulk(out, payload);
}

void erase_name(std::vector<std::string>& names, std::string_view name) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] == name) {
      names[i] = std::move(names.back());
      names.pop_back();
      return;
    }
  }
}

[[nodiscard]] std::uint8_t collect_modes(const PubSubRegistry::SubscriberSet& subscribers) {
  std::uint8_t modes = 0;
  for (const auto* session : subscribers) {
    switch (session->wire_mode) {
      case WireMode::resp3:
        modes |= kModeResp3;
        break;
      case WireMode::sbe:
        modes |= kModeSbe;
        break;
      default:
        modes |= kModeResp2;
        break;
    }
    if (modes == (kModeResp2 | kModeResp3 | kModeSbe)) {
      break;
    }
  }
  return modes;
}

void encode_literal_frames(std::uint8_t modes, std::string_view channel,
                           std::string_view payload, std::string& resp2,
                           std::string& resp3
#ifdef GOBLIN_HAS_SBE
                           ,
                           std::string& sbe
#endif
) {
  const bool need_resp2 = (modes & kModeResp2) != 0;
  const bool need_resp3 = (modes & kModeResp3) != 0;
  if (need_resp2 || need_resp3) {
    if (need_resp2 && need_resp3) {
      resp2.clear();
      append_literal_body(resp2, kResp2MessagePrefix, channel, payload);
      resp3 = resp2;
      resp3[0] = '>';
    } else if (need_resp2) {
      resp2.clear();
      append_literal_body(resp2, kResp2MessagePrefix, channel, payload);
    } else {
      resp3.clear();
      append_literal_body(resp3, kResp3MessagePrefix, channel, payload);
    }
  }
#ifdef GOBLIN_HAS_SBE
  if ((modes & kModeSbe) != 0) {
    sbe.clear();
    const std::size_t body = goblin_sbe::MessageHeader::encodedLength() +
                             goblin_sbe::PubSubPush::sbeBlockLength() + 12 +
                             channel.size() + payload.size();
    const std::size_t start = sbe.size();
    sbe.resize(start + kSbeLenPrefix + body + 16);
    goblin_sbe::PubSubPush message;
    message.wrapAndApplyHeader(sbe.data() + start, kSbeLenPrefix, sbe.size() - start)
        .kind(0)
        .subscriptionCount(0);
    message.putPattern("", 0);
    message.putChannel(channel.data(), static_cast<std::uint32_t>(channel.size()));
    message.putPayload(payload.data(), static_cast<std::uint32_t>(payload.size()));
    const auto encoded = static_cast<std::uint32_t>(
        goblin_sbe::MessageHeader::encodedLength() + message.encodedLength());
    std::memcpy(sbe.data() + start, &encoded, kSbeLenPrefix);
    sbe.resize(start + kSbeLenPrefix + encoded);
  }
#endif
}

void encode_pattern_frames(std::uint8_t modes, std::string_view pattern,
                           std::string_view channel, std::string_view payload,
                           std::string& resp2, std::string& resp3
#ifdef GOBLIN_HAS_SBE
                           ,
                           std::string& sbe
#endif
) {
  const bool need_resp2 = (modes & kModeResp2) != 0;
  const bool need_resp3 = (modes & kModeResp3) != 0;
  if (need_resp2 || need_resp3) {
    if (need_resp2 && need_resp3) {
      resp2.clear();
      append_pattern_body(resp2, kResp2PmessagePrefix, pattern, channel, payload);
      resp3 = resp2;
      resp3[0] = '>';
    } else if (need_resp2) {
      resp2.clear();
      append_pattern_body(resp2, kResp2PmessagePrefix, pattern, channel, payload);
    } else {
      resp3.clear();
      append_pattern_body(resp3, kResp3PmessagePrefix, pattern, channel, payload);
    }
  }
#ifdef GOBLIN_HAS_SBE
  if ((modes & kModeSbe) != 0) {
    sbe.clear();
    const std::size_t body = goblin_sbe::MessageHeader::encodedLength() +
                             goblin_sbe::PubSubPush::sbeBlockLength() + 12 +
                             pattern.size() + channel.size() + payload.size();
    const std::size_t start = sbe.size();
    sbe.resize(start + kSbeLenPrefix + body + 16);
    goblin_sbe::PubSubPush message;
    message.wrapAndApplyHeader(sbe.data() + start, kSbeLenPrefix, sbe.size() - start)
        .kind(1)
        .subscriptionCount(0);
    message.putPattern(pattern.data(), static_cast<std::uint32_t>(pattern.size()));
    message.putChannel(channel.data(), static_cast<std::uint32_t>(channel.size()));
    message.putPayload(payload.data(), static_cast<std::uint32_t>(payload.size()));
    const auto encoded = static_cast<std::uint32_t>(
        goblin_sbe::MessageHeader::encodedLength() + message.encodedLength());
    std::memcpy(sbe.data() + start, &encoded, kSbeLenPrefix);
    sbe.resize(start + kSbeLenPrefix + encoded);
  }
#endif
}

#ifdef GOBLIN_HAS_SBE
void append_sbe_push(std::string& out, std::uint8_t kind, std::uint32_t count,
                     std::string_view pattern, std::string_view channel,
                     std::string_view payload) {
  const std::size_t body = goblin_sbe::MessageHeader::encodedLength() +
                           goblin_sbe::PubSubPush::sbeBlockLength() + 12 +
                           pattern.size() + channel.size() + payload.size();
  const std::size_t start = out.size();
  out.resize(start + kSbeLenPrefix + body + 16);
  goblin_sbe::PubSubPush message;
  message.wrapAndApplyHeader(out.data() + start, kSbeLenPrefix,
                             out.size() - start)
      .kind(kind)
      .subscriptionCount(count);
  message.putPattern(pattern.data(), static_cast<std::uint32_t>(pattern.size()));
  message.putChannel(channel.data(), static_cast<std::uint32_t>(channel.size()));
  message.putPayload(payload.data(), static_cast<std::uint32_t>(payload.size()));
  const auto encoded = static_cast<std::uint32_t>(
      goblin_sbe::MessageHeader::encodedLength() + message.encodedLength());
  std::memcpy(out.data() + start, &encoded, kSbeLenPrefix);
  out.resize(start + kSbeLenPrefix + encoded);
}

void append_sbe_integer(std::string& out, long long value) {
  const std::size_t need = kSbeLenPrefix +
                           goblin_sbe::MessageHeader::encodedLength() +
                           goblin_sbe::IntReply::sbeBlockLength();
  const std::size_t start = out.size();
  out.resize(start + need);
  goblin_sbe::IntReply reply;
  reply.wrapAndApplyHeader(out.data() + start, kSbeLenPrefix, need).value(value);
  const auto encoded = static_cast<std::uint32_t>(
      goblin_sbe::MessageHeader::encodedLength() + reply.encodedLength());
  std::memcpy(out.data() + start, &encoded, kSbeLenPrefix);
}

void append_sbe_error(std::string& out, std::string_view message) {
  const std::size_t body = goblin_sbe::MessageHeader::encodedLength() + 8 +
                           3 + message.size();
  const std::size_t start = out.size();
  out.resize(start + kSbeLenPrefix + body + 16);
  goblin_sbe::ErrorReply reply;
  reply.wrapAndApplyHeader(out.data() + start, kSbeLenPrefix,
                           out.size() - start);
  reply.putCode("ERR", 3);
  reply.putMessage(message.data(), static_cast<std::uint32_t>(message.size()));
  const auto encoded = static_cast<std::uint32_t>(
      goblin_sbe::MessageHeader::encodedLength() + reply.encodedLength());
  std::memcpy(out.data() + start, &encoded, kSbeLenPrefix);
  out.resize(start + kSbeLenPrefix + encoded);
}

void append_sbe_string_array(std::string& out,
                             std::span<const std::string> values) {
  std::size_t body = goblin_sbe::MessageHeader::encodedLength() + 4;
  for (const auto& value : values) {
    body += 4 + value.size();
  }
  const std::size_t start = out.size();
  out.resize(start + kSbeLenPrefix + body + 16);
  goblin_sbe::ArrayReply reply;
  reply.wrapAndApplyHeader(out.data() + start, kSbeLenPrefix,
                           out.size() - start);
  auto& items = reply.itemsCount(static_cast<std::uint16_t>(values.size()));
  for (const auto& value : values) {
    items.next().putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  }
  const auto encoded = static_cast<std::uint32_t>(
      goblin_sbe::MessageHeader::encodedLength() + reply.encodedLength());
  std::memcpy(out.data() + start, &encoded, kSbeLenPrefix);
  out.resize(start + kSbeLenPrefix + encoded);
}

void append_sbe_numsub(
    std::string& out,
    std::span<const std::pair<std::string_view, std::uint32_t>> values) {
  std::size_t body = goblin_sbe::MessageHeader::encodedLength() + 4;
  for (const auto& [channel, count] : values) {
    (void)count;
    body += goblin_sbe::PubSubNumSubReply::Items::sbeBlockLength() + 4 +
            channel.size();
  }
  const std::size_t start = out.size();
  out.resize(start + kSbeLenPrefix + body + 16);
  goblin_sbe::PubSubNumSubReply reply;
  reply.wrapAndApplyHeader(out.data() + start, kSbeLenPrefix,
                           out.size() - start);
  auto& items = reply.itemsCount(static_cast<std::uint16_t>(values.size()));
  for (const auto& [channel, count] : values) {
    items.next().subscriberCount(count).putChannel(
        channel.data(), static_cast<std::uint32_t>(channel.size()));
  }
  const auto encoded = static_cast<std::uint32_t>(
      goblin_sbe::MessageHeader::encodedLength() + reply.encodedLength());
  std::memcpy(out.data() + start, &encoded, kSbeLenPrefix);
  out.resize(start + kSbeLenPrefix + encoded);
}
#endif

void append_command_error(std::string& out, WireMode mode,
                          std::string_view message) {
#ifdef GOBLIN_HAS_SBE
  if (mode == WireMode::sbe) {
    append_sbe_error(out, message);
    return;
  }
#endif
  (void)mode;
  out.push_back('-');
  out.append("ERR ", 4);
  out.append(message);
  out.append("\r\n", 2);
}

struct ClassMatch {
  bool valid{false};
  bool matches{false};
  std::size_t next{0};
};

[[nodiscard]] ClassMatch match_class(std::string_view pattern, std::size_t offset,
                                     unsigned char value) noexcept {
  std::size_t i = offset + 1;
  bool negate = false;
  if (i < pattern.size() && pattern[i] == '^') {
    negate = true;
    ++i;
  }

  bool matched = false;
  bool any = false;
  while (i < pattern.size() && pattern[i] != ']') {
    unsigned char first = static_cast<unsigned char>(pattern[i++]);
    if (first == '\\' && i < pattern.size()) {
      first = static_cast<unsigned char>(pattern[i++]);
    }
    any = true;

    if (i + 1 < pattern.size() && pattern[i] == '-' && pattern[i + 1] != ']') {
      ++i;
      unsigned char last = static_cast<unsigned char>(pattern[i++]);
      if (last == '\\' && i < pattern.size()) {
        last = static_cast<unsigned char>(pattern[i++]);
      }
      if (first <= value && value <= last) {
        matched = true;
      }
    } else if (first == value) {
      matched = true;
    }
  }

  if (i >= pattern.size() || pattern[i] != ']' || !any) {
    return {};
  }
  return {.valid = true, .matches = negate ? !matched : matched, .next = i + 1};
}

[[nodiscard]] bool match_one(std::string_view pattern, std::size_t& pattern_pos,
                             unsigned char value) noexcept {
  if (pattern_pos >= pattern.size()) {
    return false;
  }
  const char token = pattern[pattern_pos];
  if (token == '?') {
    ++pattern_pos;
    return true;
  }
  if (token == '[') {
    const auto result = match_class(pattern, pattern_pos, value);
    if (result.valid) {
      pattern_pos = result.next;
      return result.matches;
    }
  }
  if (token == '\\' && pattern_pos + 1 < pattern.size()) {
    ++pattern_pos;
  }
  return static_cast<unsigned char>(pattern[pattern_pos++]) == value;
}

[[nodiscard]] bool glob_match_general(std::string_view pattern,
                                      std::string_view value) noexcept {
  std::size_t p = 0;
  std::size_t v = 0;
  std::size_t star_pattern = std::string_view::npos;
  std::size_t star_value = 0;

  while (v < value.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      while (p < pattern.size() && pattern[p] == '*') {
        ++p;
      }
      star_pattern = p;
      star_value = v;
      continue;
    }

    std::size_t next = p;
    if (match_one(pattern, next, static_cast<unsigned char>(value[v]))) {
      p = next;
      ++v;
      continue;
    }

    if (star_pattern == std::string_view::npos) {
      return false;
    }
    p = star_pattern;
    v = ++star_value;
  }

  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

}  // namespace

UnsolicitedOutputQueue::UnsolicitedOutputQueue(std::size_t mapped_bytes)
    : capacity_(mapped_bytes) {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_POPULATE
  flags |= MAP_POPULATE;
#endif
  void* mapping = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (mapping == MAP_FAILED) {
    throw std::bad_alloc();
  }
  data_ = static_cast<char*>(mapping);

#ifndef MAP_POPULATE
  const long raw_page = ::sysconf(_SC_PAGESIZE);
  const std::size_t page =
      raw_page > 0 ? static_cast<std::size_t>(raw_page) : std::size_t{4096};
  for (std::size_t offset = 0; offset < capacity_; offset += page) {
    data_[offset] = 0;
  }
  // Production capacities are page-rounded, but touching the final byte also
  // keeps direct small-capacity unit tests honest.
  data_[capacity_ - 1] = 0;
#else
  // MAP_POPULATE faults pages in; still touch the last byte so tiny test
  // capacities that are not page-aligned stay resident.
  if (capacity_ != 0) {
    data_[capacity_ - 1] = 0;
  }
#endif
  (void)::mlock(data_, capacity_);
}

UnsolicitedOutputQueue::~UnsolicitedOutputQueue() {
  if (data_ != nullptr) {
    (void)::munmap(data_, capacity_);
  }
}

bool UnsolicitedOutputQueue::push(std::uint64_t sequence,
                                  std::string_view bytes) noexcept {
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const std::size_t record_bytes = kRecordHeaderBytes + bytes.size();
  if (record_bytes > capacity_ || capacity_ - used_bytes_ < record_bytes) {
    return false;
  }

  if (used_bytes_ == 0) {
    read_offset_ = 0;
    write_offset_ = 0;
  }

  if (write_offset_ >= read_offset_) {
    const std::size_t tail_bytes = capacity_ - write_offset_;
    if (tail_bytes < record_bytes) {
      if (capacity_ - used_bytes_ < tail_bytes + record_bytes ||
          read_offset_ < record_bytes) {
        return false;
      }
      if (tail_bytes >= sizeof(std::uint32_t)) {
        std::memcpy(data_ + write_offset_, &kWrapRecord, sizeof(kWrapRecord));
      }
      used_bytes_ += tail_bytes;
      write_offset_ = 0;
    }
  } else if (read_offset_ - write_offset_ < record_bytes) {
    return false;
  }

  const auto length = static_cast<std::uint32_t>(bytes.size());
  std::memcpy(data_ + write_offset_, &length, sizeof(length));
  std::memcpy(data_ + write_offset_ + sizeof(length), &sequence, sizeof(sequence));
  std::memcpy(data_ + write_offset_ + kRecordHeaderBytes, bytes.data(), bytes.size());
  write_offset_ += record_bytes;
  if (write_offset_ == capacity_) {
    write_offset_ = 0;
  }
  used_bytes_ += record_bytes;
  payload_bytes_ += bytes.size();
  return true;
}

void UnsolicitedOutputQueue::normalize_head() noexcept {
  while (used_bytes_ != 0) {
    const std::size_t tail_bytes = capacity_ - read_offset_;
    if (tail_bytes < sizeof(std::uint32_t)) {
      used_bytes_ -= tail_bytes;
      read_offset_ = 0;
      continue;
    }
    std::uint32_t length = 0;
    std::memcpy(&length, data_ + read_offset_, sizeof(length));
    if (length != kWrapRecord) {
      return;
    }
    used_bytes_ -= tail_bytes;
    read_offset_ = 0;
  }
}

std::optional<UnsolicitedOutputQueue::Front> UnsolicitedOutputQueue::front() noexcept {
  normalize_head();
  if (used_bytes_ == 0) {
    return std::nullopt;
  }
  std::uint32_t length = 0;
  std::uint64_t sequence = 0;
  std::memcpy(&length, data_ + read_offset_, sizeof(length));
  std::memcpy(&sequence, data_ + read_offset_ + sizeof(length), sizeof(sequence));
  return Front{.sequence = sequence,
               .bytes = std::string_view(data_ + read_offset_ + kRecordHeaderBytes,
                                         length)};
}

void UnsolicitedOutputQueue::pop_front(std::size_t payload_len) noexcept {
  const std::size_t record_bytes = kRecordHeaderBytes + payload_len;
  read_offset_ += record_bytes;
  if (read_offset_ == capacity_) {
    read_offset_ = 0;
  }
  used_bytes_ -= record_bytes;
  payload_bytes_ -= payload_len;
  if (used_bytes_ == 0) {
    read_offset_ = 0;
    write_offset_ = 0;
  }
}

void UnsolicitedOutputQueue::pop() noexcept {
  normalize_head();
  if (used_bytes_ == 0) {
    return;
  }
  std::uint32_t length = 0;
  std::memcpy(&length, data_ + read_offset_, sizeof(length));
  pop_front(length);
}

void UnsolicitedOutputQueue::clear() noexcept {
  read_offset_ = 0;
  write_offset_ = 0;
  used_bytes_ = 0;
  payload_bytes_ = 0;
}

PatternKind PubSubRegistry::classify_pattern(std::string_view pattern) noexcept {
  if (pattern == "*") {
    return PatternKind::always;
  }

  std::size_t star_count = 0;
  std::size_t star_pos = 0;
  for (std::size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    // Escapes need the general matcher (e.g. "literal\*" matches "literal*").
    if (c == '\\' || c == '?' || c == '[') {
      return PatternKind::general;
    }
    if (c == '*') {
      ++star_count;
      star_pos = i;
      if (star_count > 1) {
        return PatternKind::general;
      }
    }
  }

  if (star_count == 0) {
    return PatternKind::literal;
  }
  if (star_pos == pattern.size() - 1) {
    return PatternKind::prefix;
  }
  if (star_pos == 0) {
    return PatternKind::suffix;
  }
  return PatternKind::prefix_suffix;
}

bool PubSubRegistry::match_classified(PatternKind kind, std::string_view pattern,
                                      std::string_view value) noexcept {
  switch (kind) {
    case PatternKind::always:
      return true;
    case PatternKind::literal:
      return pattern == value;
    case PatternKind::prefix: {
      const std::string_view prefix = pattern.substr(0, pattern.size() - 1);
      return value.size() >= prefix.size() &&
             value.compare(0, prefix.size(), prefix) == 0;
    }
    case PatternKind::suffix: {
      const std::string_view suffix = pattern.substr(1);
      return value.size() >= suffix.size() &&
             value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
                 0;
    }
    case PatternKind::prefix_suffix: {
      const auto star = pattern.find('*');
      const std::string_view prefix = pattern.substr(0, star);
      const std::string_view suffix = pattern.substr(star + 1);
      if (value.size() < prefix.size() + suffix.size()) {
        return false;
      }
      return value.compare(0, prefix.size(), prefix) == 0 &&
             value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
                 0;
    }
    case PatternKind::general:
      return glob_match_general(pattern, value);
  }
  return false;
}

bool PubSubRegistry::glob_match(std::string_view pattern,
                                std::string_view value) noexcept {
  return match_classified(classify_pattern(pattern), pattern, value);
}

bool PubSubRegistry::enqueue(PubSubSession& session, std::string_view bytes) {
  const std::uint64_t sequence = session.next_output_sequence;
  if (session.unsolicited.push(sequence, bytes)) {
    ++session.next_output_sequence;
    // Push appends at the tail; a cached head remains valid.
    return true;
  }
  if (!session.close_requested) {
    session.close_requested = true;
    overflowed_.push_back(&session);
  }
  return false;
}

void PubSubRegistry::append_ack(std::string& out, const PubSubSession& session,
                                AckKind kind,
                                std::optional<std::string_view> name) const {
#ifdef GOBLIN_HAS_SBE
  if (session.wire_mode == WireMode::sbe) {
    const bool pattern =
        kind == AckKind::psubscribe || kind == AckKind::punsubscribe;
    append_sbe_push(
        out, static_cast<std::uint8_t>(kind),
        static_cast<std::uint32_t>(session.subscription_count()),
        pattern ? name.value_or(std::string_view{}) : std::string_view{},
        pattern ? std::string_view{} : name.value_or(std::string_view{}), {});
    return;
  }
#endif
  std::string_view kind_text;
  switch (kind) {
    case AckKind::subscribe:
      kind_text = "subscribe";
      break;
    case AckKind::psubscribe:
      kind_text = "psubscribe";
      break;
    case AckKind::unsubscribe:
      kind_text = "unsubscribe";
      break;
    case AckKind::punsubscribe:
      kind_text = "punsubscribe";
      break;
  }
  if (session.wire_mode == WireMode::resp3) {
    resp::append_push_header(out, 3);
  } else {
    resp::append_array_header(out, 3);
  }
  resp::append_bulk_string(out, kind_text);
  if (name) {
    resp::append_bulk_string(out, *name);
  } else {
    resp::append_null(out, session.wire_mode == WireMode::resp3
                               ? resp::Version::resp3
                               : resp::Version::resp2);
  }
  resp::append_integer(out, static_cast<long long>(session.subscription_count()));
}

void PubSubRegistry::erase_pattern_publish_entry(std::string_view pattern) {
  for (std::size_t i = 0; i < pattern_publish_.size(); ++i) {
    if (pattern_publish_[i].pattern == pattern) {
      pattern_publish_[i] = std::move(pattern_publish_.back());
      pattern_publish_.pop_back();
      return;
    }
  }
}

void PubSubRegistry::subscribe(PubSubSession& session,
                               std::span<const std::string_view> names,
                               bool patterns, std::string& out) {
  auto& table = patterns ? patterns_ : channels_;
  auto& count = patterns ? session.pattern_subscriptions
                         : session.literal_subscriptions;
  auto& reverse = patterns ? session.pattern_names : session.channel_names;
  const AckKind kind = patterns ? AckKind::psubscribe : AckKind::subscribe;
  for (const auto name : names) {
    // Pass string_view so a hit does not allocate a temporary key string.
    auto [subscribers, inserted_name] = table.try_emplace(name);
    if (inserted_name && patterns) {
      pattern_publish_.push_back(
          PatternPublishEntry{.pattern = std::string(name),
                              .kind = classify_pattern(name)});
    }
    if (subscribers->insert(&session).second) {
      ++count;
      reverse.emplace_back(name);
    }
    append_ack(out, session, kind, name);
  }
}

void PubSubRegistry::unsubscribe(PubSubSession& session,
                                 std::span<const std::string_view> names,
                                 bool patterns, std::string& out) {
  auto& table = patterns ? patterns_ : channels_;
  auto& count = patterns ? session.pattern_subscriptions
                         : session.literal_subscriptions;
  auto& reverse = patterns ? session.pattern_names : session.channel_names;
  const AckKind kind = patterns ? AckKind::punsubscribe : AckKind::unsubscribe;

  if (names.empty()) {
    if (reverse.empty()) {
      append_ack(out, session, kind, std::nullopt);
      return;
    }
    // Snapshot names: reverse is mutated as we remove each subscription.
    std::vector<std::string> targets = reverse;
    for (const auto& name : targets) {
      bool empty = false;
      if (auto* subscribers = table.find(std::string_view(name))) {
        if (subscribers->erase(&session) != 0) {
          --count;
        }
        empty = subscribers->empty();
      }
      erase_name(reverse, name);
      if (empty) {
        table.erase(std::string_view(name));
        if (patterns) {
          erase_pattern_publish_entry(name);
        }
      }
      append_ack(out, session, kind, name);
    }
    return;
  }

  for (const auto name : names) {
    bool empty = false;
    if (auto* subscribers = table.find(name)) {
      if (subscribers->erase(&session) != 0) {
        --count;
        erase_name(reverse, name);
      }
      empty = subscribers->empty();
    }
    if (empty) {
      table.erase(name);
      if (patterns) {
        erase_pattern_publish_entry(name);
      }
    }
    append_ack(out, session, kind, name);
  }
}

void PubSubRegistry::deliver_to_set(const SubscriberSet& subscribers,
                                    std::uint8_t modes, long long& deliveries) {
  std::string_view by_mode[4]{};
  if ((modes & kModeResp2) != 0) {
    by_mode[static_cast<unsigned>(WireMode::undecided)] = resp2_scratch_;
    by_mode[static_cast<unsigned>(WireMode::resp2)] = resp2_scratch_;
  }
  if ((modes & kModeResp3) != 0) {
    by_mode[static_cast<unsigned>(WireMode::resp3)] = resp3_scratch_;
  }
#ifdef GOBLIN_HAS_SBE
  if ((modes & kModeSbe) != 0) {
    by_mode[static_cast<unsigned>(WireMode::sbe)] = sbe_scratch_;
  }
#endif

  const auto begin = subscribers.begin();
  const auto end = subscribers.end();
  for (auto it = begin; it != end; ++it) {
    auto next = it;
    ++next;
    if (next != end) {
#if defined(__GNUC__) || defined(__clang__)
      __builtin_prefetch(static_cast<const void*>(*next), 1, 1);
#endif
    }
    auto* session = *it;
    const auto mode_index = static_cast<unsigned>(session->wire_mode);
    const std::string_view bytes = by_mode[mode_index];
    if (bytes.data() == nullptr) {
      // Mode was not present in the pre-scan (e.g. concurrent wire-mode change
      // mid-publish is impossible on the single-threaded server; undecided with
      // only RESP3 peers). Fall back to RESP2 if encoded, else skip encode path
      // already covered — re-encode is not needed; use resp2 if available.
      const std::string_view fallback =
          (modes & kModeResp2) != 0
              ? std::string_view(resp2_scratch_)
              : ((modes & kModeResp3) != 0 ? std::string_view(resp3_scratch_)
                                           : std::string_view{});
      if (fallback.empty()) {
        continue;
      }
      if (enqueue(*session, fallback)) {
        ++deliveries;
      }
      continue;
    }
    if (enqueue(*session, bytes)) {
      ++deliveries;
    }
  }
}

long long PubSubRegistry::publish(std::string_view channel,
                                  std::string_view payload) {
  long long deliveries = 0;

  if (auto* subscribers = channels_.find(channel)) {
    if (!subscribers->empty()) {
      const std::uint8_t modes = collect_modes(*subscribers);
      encode_literal_frames(modes, channel, payload, resp2_scratch_, resp3_scratch_
#ifdef GOBLIN_HAS_SBE
                            ,
                            sbe_scratch_
#endif
      );
      deliver_to_set(*subscribers, modes, deliveries);
    }
  }

  if (!pattern_publish_.empty()) {
    for (const auto& entry : pattern_publish_) {
      if (!match_classified(entry.kind, entry.pattern, channel)) {
        continue;
      }
      auto* subscribers = patterns_.find(std::string_view(entry.pattern));
      if (subscribers == nullptr || subscribers->empty()) {
        continue;
      }
      const std::uint8_t modes = collect_modes(*subscribers);
      encode_pattern_frames(modes, entry.pattern, channel, payload, resp2_scratch_,
                            resp3_scratch_
#ifdef GOBLIN_HAS_SBE
                            ,
                            sbe_scratch_
#endif
      );
      deliver_to_set(*subscribers, modes, deliveries);
    }
  }

  cleanup_overflowed();
  return deliveries;
}

void PubSubRegistry::cleanup_overflowed() {
  for (auto* session : overflowed_) {
    remove(*session);
  }
  overflowed_.clear();
}

void PubSubRegistry::remove(PubSubSession& session) {
  if (session.subscription_count() == 0 && session.channel_names.empty() &&
      session.pattern_names.empty()) {
    session.unsolicited.clear();
    session.clear_unsolicited_front_cache();
    return;
  }

  for (const auto& name : session.channel_names) {
    if (auto* subscribers = channels_.find(std::string_view(name))) {
      (void)subscribers->erase(&session);
      if (subscribers->empty()) {
        channels_.erase(std::string_view(name));
      }
    }
  }
  session.channel_names.clear();
  session.literal_subscriptions = 0;

  for (const auto& name : session.pattern_names) {
    if (auto* subscribers = patterns_.find(std::string_view(name))) {
      (void)subscribers->erase(&session);
      if (subscribers->empty()) {
        patterns_.erase(std::string_view(name));
        erase_pattern_publish_entry(name);
      }
    }
  }
  session.pattern_names.clear();
  session.pattern_subscriptions = 0;

  session.unsolicited.clear();
  session.clear_unsolicited_front_cache();
}

void PubSubRegistry::execute(PubSubSession& session, const Command& command,
                             std::string& out) {
  switch (command.type) {
    case CommandType::subscribe:
      if (command.args.empty()) {
        append_command_error(out, session.wire_mode,
                             "SUBSCRIBE requires at least one channel");
        return;
      }
      subscribe(session, command.args, false, out);
      return;
    case CommandType::psubscribe:
      if (command.args.empty()) {
        append_command_error(out, session.wire_mode,
                             "PSUBSCRIBE requires at least one pattern");
        return;
      }
      subscribe(session, command.args, true, out);
      return;
    case CommandType::unsubscribe:
      unsubscribe(session, command.args, false, out);
      return;
    case CommandType::punsubscribe:
      unsubscribe(session, command.args, true, out);
      return;
    case CommandType::publish: {
      const auto delivered = publish(command.args[0], command.args[1]);
#ifdef GOBLIN_HAS_SBE
      if (session.wire_mode == WireMode::sbe) {
        append_sbe_integer(out, delivered);
        return;
      }
#endif
      resp::append_integer(out, delivered);
      return;
    }
    case CommandType::pubsub:
      break;
    default:
      return;
  }

  const auto subcommand = command.args[0];
  // Length-gated dispatch for the three fixed PUBSUB subcommands.
  if (subcommand.size() == 8 && equals_ci(subcommand, "channels")) {
    if (command.args.size() > 2) {
      append_command_error(out, session.wire_mode,
                           "wrong number of arguments for 'pubsub|channels' command");
      return;
    }
    const std::string_view filter =
        command.args.size() == 2 ? command.args[1] : "*";
    const bool match_all = filter == "*";
    const PatternKind filter_kind =
        match_all ? PatternKind::always : classify_pattern(filter);
    std::vector<std::string> names;
    names.reserve(channels_.size());
    channels_.for_each([&](const auto& entry) {
      if (match_all || match_classified(filter_kind, filter, entry.first)) {
        names.push_back(entry.first);
      }
    });
#ifdef GOBLIN_HAS_SBE
    if (session.wire_mode == WireMode::sbe) {
      if (names.size() > std::numeric_limits<std::uint16_t>::max()) {
        append_sbe_error(out, "PUBSUB CHANNELS result exceeds the SBE group limit");
        return;
      }
      append_sbe_string_array(out, names);
      return;
    }
#endif
    resp::append_array_header(out, names.size());
    for (const auto& name : names) {
      resp::append_bulk_string(out, name);
    }
    return;
  }

  if (subcommand.size() == 6 && equals_ci(subcommand, "numpat")) {
    if (command.args.size() != 1) {
      append_command_error(out, session.wire_mode,
                           "wrong number of arguments for 'pubsub|numpat' command");
      return;
    }
#ifdef GOBLIN_HAS_SBE
    if (session.wire_mode == WireMode::sbe) {
      append_sbe_integer(out, static_cast<long long>(patterns_.size()));
      return;
    }
#endif
    resp::append_integer(out, static_cast<long long>(patterns_.size()));
    return;
  }

  if (subcommand.size() == 6 && equals_ci(subcommand, "numsub")) {
    std::vector<std::pair<std::string_view, std::uint32_t>> values;
    values.reserve(command.args.size() - 1);
    for (std::size_t i = 1; i < command.args.size(); ++i) {
      const auto* subscribers = channels_.find(command.args[i]);
      values.emplace_back(command.args[i], subscribers == nullptr
                                               ? 0U
                                               : static_cast<std::uint32_t>(
                                                     subscribers->size()));
    }
#ifdef GOBLIN_HAS_SBE
    if (session.wire_mode == WireMode::sbe) {
      if (values.size() > std::numeric_limits<std::uint16_t>::max()) {
        append_sbe_error(out, "PUBSUB NUMSUB request exceeds the SBE group limit");
      } else {
        append_sbe_numsub(out, values);
      }
      return;
    }
#endif
    resp::append_array_header(out, (command.args.size() - 1) * 2);
    for (const auto& [channel, count] : values) {
      resp::append_bulk_string(out, channel);
      resp::append_integer(out, count);
    }
    return;
  }

  append_command_error(out, session.wire_mode, "unknown PUBSUB subcommand");
}

}  // namespace goblin::core::detail
