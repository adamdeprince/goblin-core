#include "transaction.hpp"

#include "goblin/core/auth.hpp"

#include <cstring>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace goblin::core::detail {
namespace {

constexpr std::size_t kRecordHeaderBytes = sizeof(std::uint32_t) * 2;
constexpr std::size_t kFieldHeaderBytes = sizeof(std::uint32_t);

template <class T>
void write_unaligned(char* destination, T value) noexcept {
  std::memcpy(destination, &value, sizeof(value));
}

template <class T>
[[nodiscard]] T read_unaligned(const char* source) noexcept {
  T value{};
  std::memcpy(&value, source, sizeof(value));
  return value;
}

}  // namespace

TransactionBuffer::TransactionBuffer(std::size_t mapped_bytes)
    : capacity_(mapped_bytes) {
  if (capacity_ == 0) {
    throw std::bad_alloc();
  }
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_POPULATE
  flags |= MAP_POPULATE;
#endif
  void* mapping =
      ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (mapping == MAP_FAILED) {
    throw std::bad_alloc();
  }
  data_ = static_cast<char*>(mapping);

  const long native_page = ::sysconf(_SC_PAGESIZE);
  const std::size_t page =
      native_page > 0 ? static_cast<std::size_t>(native_page) : 4096U;
  for (std::size_t offset = 0; offset < capacity_; offset += page) {
    data_[offset] = 0;
  }
  data_[capacity_ - 1] = 0;
  (void)::mlock(data_, capacity_);
}

TransactionBuffer::~TransactionBuffer() {
  if (data_ != nullptr) {
    secure_zero_memory(data_, used_bytes_);
    (void)::munlock(data_, capacity_);
    (void)::munmap(data_, capacity_);
  }
}

bool TransactionBuffer::append(
    std::span<const std::string_view> fields) noexcept {
  if (fields.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  std::size_t record_bytes = kRecordHeaderBytes;
  for (const auto field : fields) {
    constexpr std::size_t kMaxRecord =
        std::numeric_limits<std::uint32_t>::max();
    if (field.size() > kMaxRecord - kFieldHeaderBytes ||
        record_bytes > kMaxRecord - kFieldHeaderBytes - field.size()) {
      return false;
    }
    record_bytes += kFieldHeaderBytes + field.size();
  }
  if (record_bytes > capacity_ - used_bytes_) {
    return false;
  }

  char* cursor = data_ + used_bytes_;
  write_unaligned(cursor, static_cast<std::uint32_t>(record_bytes));
  cursor += sizeof(std::uint32_t);
  write_unaligned(cursor, static_cast<std::uint32_t>(fields.size()));
  cursor += sizeof(std::uint32_t);
  for (const auto field : fields) {
    write_unaligned(cursor, static_cast<std::uint32_t>(field.size()));
    cursor += sizeof(std::uint32_t);
    if (!field.empty()) {
      std::memcpy(cursor, field.data(), field.size());
    }
    cursor += field.size();
  }
  used_bytes_ += record_bytes;
  ++command_count_;
  return true;
}

bool TransactionBuffer::decode(
    std::size_t& offset, std::vector<std::string_view>& fields) const {
  fields.clear();
  if (offset >= used_bytes_ || used_bytes_ - offset < kRecordHeaderBytes) {
    return false;
  }

  const char* const record = data_ + offset;
  const auto record_bytes = read_unaligned<std::uint32_t>(record);
  const auto field_count =
      read_unaligned<std::uint32_t>(record + sizeof(std::uint32_t));
  if (record_bytes < kRecordHeaderBytes || record_bytes > used_bytes_ - offset) {
    return false;
  }

  const char* cursor = record + kRecordHeaderBytes;
  const char* const end = record + record_bytes;
  fields.reserve(field_count);
  for (std::uint32_t i = 0; i < field_count; ++i) {
    if (static_cast<std::size_t>(end - cursor) < kFieldHeaderBytes) {
      return false;
    }
    const auto field_bytes = read_unaligned<std::uint32_t>(cursor);
    cursor += sizeof(std::uint32_t);
    if (static_cast<std::size_t>(end - cursor) < field_bytes) {
      return false;
    }
    fields.emplace_back(cursor, field_bytes);
    cursor += field_bytes;
  }
  if (cursor != end) {
    return false;
  }
  offset += record_bytes;
  return true;
}

void TransactionBuffer::clear() noexcept {
  secure_zero_memory(data_, used_bytes_);
  used_bytes_ = 0;
  command_count_ = 0;
}

void WatchRegistry::watch(TransactionState& state,
                          std::span<const std::string_view> keys) {
  try {
    state.has_watches = true;
    for (const auto key : keys) {
      auto [watchers, inserted] = watched_.try_emplace(key);
      (void)inserted;
      watchers->insert(&state);
    }
  } catch (...) {
    remove(state);
    throw;
  }
}

void WatchRegistry::remove(TransactionState& state) noexcept {
  if (!state.has_watches) {
    state.watch_dirty = false;
    return;
  }
  watched_.for_each([&state](auto& entry) { entry.second.erase(&state); });
  (void)watched_.erase_if(
      [](const auto& entry) { return entry.second.empty(); });
  if (watched_.empty()) {
    watched_ = WatchTable{};
  }
  state.has_watches = false;
  state.watch_dirty = false;
}

void WatchRegistry::modified(std::string_view key) noexcept {
  auto* watchers = watched_.find(key);
  if (watchers == nullptr) {
    return;
  }
  for (auto* state : *watchers) {
    state->watch_dirty = true;
  }
}

void WatchRegistry::modified_all() noexcept {
  watched_.for_each([](auto& entry) {
    for (auto* state : entry.second) {
      state->watch_dirty = true;
    }
  });
}

}  // namespace goblin::core::detail
