#pragma once

#include "goblin/core/command.hpp"
#ifndef GOBLIN_BLUEFIELD_STANDALONE
#include "goblin/core/swiss_table.hpp"
#endif

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core::detail {

#ifdef GOBLIN_BLUEFIELD_STANDALONE
struct BluefieldStringHash {
  using is_transparent = void;
  using is_avalanching = void;

  [[nodiscard]] std::uint64_t operator()(std::string_view value) const noexcept {
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(value));
  }
};

struct BluefieldStringEqual {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs,
                                std::string_view rhs) const noexcept {
    return lhs == rhs;
  }
};

// The DPU process needs only Pub/Sub. This adapter keeps PubSubRegistry's
// pointer-returning table API while replacing the full server's arena-backed
// Swiss table with the smaller vendored dense map.
template <class T>
class BluefieldDenseTable {
 public:
  template <class K>
  [[nodiscard]] T* find(const K& key) noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
  }

  template <class K>
  [[nodiscard]] const T* find(const K& key) const noexcept {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : &found->second;
  }

  template <class K, class... Args>
  std::pair<T*, bool> try_emplace(const K& key, Args&&... args) {
    if (auto* value = find(key)) {
      return {value, false};
    }
    auto [position, inserted] = values_.emplace(
        std::string(std::string_view(key)), T(std::forward<Args>(args)...));
    return {&position->second, inserted};
  }

  template <class K>
  bool erase(const K& key) {
    return values_.erase(key) != 0;
  }

  void clear() noexcept { values_.clear(); }
  [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
  void reserve(std::size_t count) { values_.reserve(count); }

  template <class Fn>
  void for_each(Fn&& fn) {
    for (auto& entry : values_) fn(entry);
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (const auto& entry : values_) fn(entry);
  }

 private:
  ankerl::unordered_dense::map<std::string, T, BluefieldStringHash,
                               BluefieldStringEqual>
      values_;
};
#endif

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

#ifdef GOBLIN_BLUEFIELD_STANDALONE
using BluefieldSubscriptionWeights = BluefieldDenseTable<std::uint32_t>;
#else
using BluefieldSubscriptionWeights =
    SwissTable<std::string, std::uint32_t, StringTableHash, StringTableEqual>;
#endif

struct BluefieldAggregateState {
  BluefieldSubscriptionWeights channels;
  BluefieldSubscriptionWeights patterns;
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
  // BlueField edge links use one upstream session to represent every local
  // subscriber. The edge id joins that aggregate subscription session to the
  // separate publisher session, allowing an edge-originated publication to be
  // delivered on the host without echoing over PCIe to the originating DPU.
  std::uint64_t bluefield_edge_id{0};
  bool bluefield_aggregate{false};
  std::unique_ptr<BluefieldAggregateState> bluefield_aggregate_state;
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

#ifdef GOBLIN_BLUEFIELD_STANDALONE
// The DPU edge normally drains a freshly encoded push straight into a
// non-blocking userspace-TCP socket. Retain the mmap queue as the partial-write
// and backpressure fallback, but avoid copying every uncontended notification
// into it first.
struct PubSubDirectWriteResult {
  std::size_t consumed{0};
  bool fatal{false};
};

struct PubSubDirectWriter {
  void* context{nullptr};
  PubSubDirectWriteResult (*write)(void* context, PubSubSession& session,
                                   std::string_view bytes){nullptr};
};
#endif

struct PubSubSubscriptionObserver {
  void* context{nullptr};
  void (*changed)(void* context, std::string_view name, bool pattern,
                  std::size_t subscribers){nullptr};
};

class PubSubRegistry {
 public:
  using SubscriberSet = ankerl::unordered_dense::set<PubSubSession*>;
#ifdef GOBLIN_BLUEFIELD_STANDALONE
  using SubscriptionTable = BluefieldDenseTable<SubscriberSet>;
#else
  using SubscriptionTable =
      SwissTable<std::string, SubscriberSet, StringTableHash, StringTableEqual>;
#endif

  void execute(PubSubSession& session, const Command& command, std::string& out);
  void remove(PubSubSession& session);
  [[nodiscard]] long long publish(std::string_view channel,
                                  std::string_view payload,
                                  std::uint64_t excluded_bluefield_edge_id = 0);
  [[nodiscard]] bool set_bluefield_subscription_weight(
      PubSubSession& session, std::string_view name, bool pattern,
      std::uint32_t subscribers);
  void set_subscription_observer(
      PubSubSubscriptionObserver observer) noexcept {
    subscription_observer_ = observer;
  }
#ifdef GOBLIN_BLUEFIELD_STANDALONE
  void set_direct_writer(PubSubDirectWriter writer) noexcept {
    direct_writer_ = writer;
  }
#endif

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
                      std::uint64_t excluded_bluefield_edge_id,
                      long long& deliveries);
  void notify_subscription_changed(std::string_view name, bool pattern,
                                   std::size_t subscribers);
  [[nodiscard]] static std::uint32_t bluefield_subscription_weight(
      const PubSubSession& session, std::string_view name, bool pattern);
  [[nodiscard]] static std::uint32_t subscriber_count(
      const SubscriberSet& subscribers, std::string_view name, bool pattern);

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
  ankerl::unordered_dense::set<PubSubSession*> aggregate_delivered_;
  PubSubSubscriptionObserver subscription_observer_{};
#ifdef GOBLIN_BLUEFIELD_STANDALONE
  PubSubDirectWriter direct_writer_{};
#endif
};

}  // namespace goblin::core::detail
