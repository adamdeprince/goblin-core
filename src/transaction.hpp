#pragma once

#include "goblin/core/swiss_table.hpp"

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core::detail {

class TransactionBuffer {
 public:
  explicit TransactionBuffer(std::size_t mapped_bytes);
  ~TransactionBuffer();

  TransactionBuffer(const TransactionBuffer&) = delete;
  TransactionBuffer& operator=(const TransactionBuffer&) = delete;
  TransactionBuffer(TransactionBuffer&&) = delete;
  TransactionBuffer& operator=(TransactionBuffer&&) = delete;

  [[nodiscard]] bool append(
      std::span<const std::string_view> fields) noexcept;
  [[nodiscard]] bool decode(std::size_t& offset,
                            std::vector<std::string_view>& fields) const;
  void clear() noexcept;

  [[nodiscard]] std::size_t mapped_bytes() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] std::size_t command_count() const noexcept {
    return command_count_;
  }

 private:
  char* data_{nullptr};
  std::size_t capacity_{0};
  std::size_t used_bytes_{0};
  std::size_t command_count_{0};
};

enum class TransactionFailure : std::uint8_t {
  none,
  command_error,
  buffer_limit,
};

struct TransactionState {
  explicit TransactionState(std::size_t mapped_bytes) : commands(mapped_bytes) {}

  void begin() noexcept {
    commands.clear();
    in_multi = true;
    failure = TransactionFailure::none;
  }

  void finish() noexcept {
    commands.clear();
    in_multi = false;
    failure = TransactionFailure::none;
  }

  void reset() noexcept {
    finish();
    watch_dirty = false;
    has_watches = false;
  }

  TransactionBuffer commands;
  bool in_multi{false};
  bool watch_dirty{false};
  bool has_watches{false};
  TransactionFailure failure{TransactionFailure::none};
};

class WatchRegistry {
 public:
  using WatcherSet = ankerl::unordered_dense::set<TransactionState*>;
  using WatchTable =
      SwissTable<std::string, WatcherSet, StringTableHash, StringTableEqual>;

  void watch(TransactionState& state,
             std::span<const std::string_view> keys);
  void remove(TransactionState& state) noexcept;
  void modified(std::string_view key) noexcept;
  void modified_all() noexcept;

  [[nodiscard]] bool empty() const noexcept { return watched_.empty(); }

 private:
  WatchTable watched_;
};

}  // namespace goblin::core::detail
