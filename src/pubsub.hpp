#pragma once

#include "goblin/core/command.hpp"
#include "goblin/core/swiss_table.hpp"

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core::detail {

enum class WireMode : std::uint8_t { undecided, resp2, resp3, sbe };

enum class AckKind : std::uint8_t {
  subscribe = 2,
  psubscribe = 3,
  unsubscribe = 4,
  punsubscribe = 5,
};

enum class PatternKind : std::uint8_t {
  always,         // "*"
  literal,        // no metacharacters
  prefix,         // "foo*" with no other meta
  suffix,         // "*foo"
  prefix_suffix,  // "foo*bar"
  general,
};

class UnsolicitedOutputQueue {
 public:
  struct Front {
    std::uint64_t sequence{0};
    std::string_view bytes;
  };

  explicit UnsolicitedOutputQueue(std::size_t mapped_bytes);
  ~UnsolicitedOutputQueue();

  UnsolicitedOutputQueue(const UnsolicitedOutputQueue&) = delete;
  UnsolicitedOutputQueue& operator=(const UnsolicitedOutputQueue&) = delete;
  UnsolicitedOutputQueue(UnsolicitedOutputQueue&&) = delete;
  UnsolicitedOutputQueue& operator=(UnsolicitedOutputQueue&&) = delete;

  [[nodiscard]] bool push(std::uint64_t sequence, std::string_view bytes) noexcept;
  [[nodiscard]] std::optional<Front> front() noexcept;
  // Drop the current head. Prefer pop_front after a successful front() so the
  // length/sequence header is not re-read.
  void pop() noexcept;
  void pop_front(std::size_t payload_len) noexcept;
  void clear() noexcept;

  [[nodiscard]] bool empty() const noexcept { return used_bytes_ == 0; }
  [[nodiscard]] std::size_t mapped_bytes() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] std::size_t payload_bytes() const noexcept { return payload_bytes_; }

 private:
  static constexpr std::uint32_t kWrapRecord = 0xFFFFFFFFU;
  static constexpr std::size_t kRecordHeaderBytes = sizeof(std::uint32_t) +
                                                     sizeof(std::uint64_t);

  void normalize_head() noexcept;

  char* data_{nullptr};
  std::size_t capacity_{0};
  std::size_t read_offset_{0};
  std::size_t write_offset_{0};
  std::size_t used_bytes_{0};
  std::size_t payload_bytes_{0};
};

struct PubSubSession {
  explicit PubSubSession(std::size_t unsolicited_output_bytes)
      : unsolicited(unsolicited_output_bytes) {}

  WireMode wire_mode{WireMode::undecided};
  UnsolicitedOutputQueue unsolicited;
  std::size_t literal_subscriptions{0};
  std::size_t pattern_subscriptions{0};
  std::uint64_t next_output_sequence{1};
  std::size_t unsolicited_front_offset{0};
  // Cached head of the unsolicited queue for partial send/CQ pushes. Valid until
  // pop_front/clear; the string_view aliases the mmap record.
  bool has_unsolicited_front{false};
  UnsolicitedOutputQueue::Front unsolicited_front{};
  // Reverse indexes: O(subs) disconnect / unsubscribe-all instead of O(all channels).
  std::vector<std::string> channel_names;
  std::vector<std::string> pattern_names;
  bool close_requested{false};

  [[nodiscard]] std::size_t subscription_count() const noexcept {
    return literal_subscriptions + pattern_subscriptions;
  }

  void clear_unsolicited_front_cache() noexcept {
    has_unsolicited_front = false;
    unsolicited_front = {};
    unsolicited_front_offset = 0;
  }
};

class PubSubRegistry {
 public:
  using SubscriberSet = ankerl::unordered_dense::set<PubSubSession*>;
  using SubscriptionTable =
      SwissTable<std::string, SubscriberSet, StringTableHash, StringTableEqual>;

  void execute(PubSubSession& session, const Command& command, std::string& out);
  void remove(PubSubSession& session);
  [[nodiscard]] long long publish(std::string_view channel,
                                  std::string_view payload);

  [[nodiscard]] static bool glob_match(std::string_view pattern,
                                       std::string_view value) noexcept;
  [[nodiscard]] static PatternKind classify_pattern(std::string_view pattern) noexcept;
  [[nodiscard]] static bool match_classified(PatternKind kind,
                                             std::string_view pattern,
                                             std::string_view value) noexcept;

 private:
  struct PatternPublishEntry {
    std::string pattern;
    PatternKind kind{PatternKind::general};
  };

  void subscribe(PubSubSession& session, std::span<const std::string_view> names,
                 bool patterns, std::string& out);
  void unsubscribe(PubSubSession& session, std::span<const std::string_view> names,
                   bool patterns, std::string& out);
  void append_ack(std::string& out, const PubSubSession& session, AckKind kind,
                  std::optional<std::string_view> name) const;
  [[nodiscard]] bool enqueue(PubSubSession& session, std::string_view bytes);
  void cleanup_overflowed();
  void erase_pattern_publish_entry(std::string_view pattern);
  void deliver_to_set(const SubscriberSet& subscribers, std::uint8_t modes,
                      long long& deliveries);

  SubscriptionTable channels_;
  SubscriptionTable patterns_;
  // Dense list of active patterns for O(active) publish scans (not table capacity).
  std::vector<PatternPublishEntry> pattern_publish_;
  std::vector<PubSubSession*> overflowed_;
  std::string resp2_scratch_;
  std::string resp3_scratch_;
#ifdef GOBLIN_HAS_SBE
  std::string sbe_scratch_;
#endif
};

}  // namespace goblin::core::detail
