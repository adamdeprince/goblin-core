#include "goblin/core/store.hpp"

#include <algorithm>
#include <cassert>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <istream>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace goblin::core {
namespace {

constexpr std::size_t kMinRemovedForCompactionCheck = 4096;
constexpr std::size_t kMinFreeSlotsForCompaction = 4096;
constexpr std::size_t kCompactionFreeSlotsPerAllocatedSlot = 4;

[[nodiscard]] long long normalize_index(long long index, std::size_t size) noexcept {
  if (index < 0) {
    index += static_cast<long long>(size);
  }
  return index;
}

void release_unused_heap_pages() noexcept {
#if defined(__APPLE__)
  (void)malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
  (void)malloc_trim(0);
#endif
}

}  // namespace

ZSet::ZSet(ZSetOptions options)
    : member_storage_(std::make_unique<ZSetMemberStorage>(options.score_string_cache)),
      members_(member_storage_.get(), options.member_index_growth),
      entries_(member_storage_.get(), options.rank_cache_mode),
      options_(options) {}

ZSet::ZSet(ZSet&& other) noexcept
    : member_storage_(std::move(other.member_storage_)),
      members_(std::move(other.members_)),
      entries_(std::move(other.entries_)),
      options_(other.options_) {
  if (member_storage_ == nullptr) {
    member_storage_ = std::make_unique<ZSetMemberStorage>();
  }
  if (other.member_storage_ == nullptr) {
    other.member_storage_ = std::make_unique<ZSetMemberStorage>();
  }
  rebind_indexes();
  other.rebind_indexes();
}

ZSet& ZSet::operator=(ZSet&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  member_storage_ = std::move(other.member_storage_);
  members_ = std::move(other.members_);
  entries_ = std::move(other.entries_);
  options_ = other.options_;
  if (member_storage_ == nullptr) {
    member_storage_ = std::make_unique<ZSetMemberStorage>();
  }
  if (other.member_storage_ == nullptr) {
    other.member_storage_ = std::make_unique<ZSetMemberStorage>();
  }
  rebind_indexes();
  other.rebind_indexes();
  return *this;
}

bool ZSet::empty() const noexcept {
  return entries_.empty();
}

std::size_t ZSet::size() const noexcept {
  return entries_.size();
}

std::size_t ZSet::block_count() const noexcept {
  return entries_.block_count();
}

std::uint32_t ZSet::allocate_member_id(std::string_view member, double score) {
  return member_storage_->push_back(member, score);
}

std::string_view ZSet::member_view(std::uint32_t member_id) const noexcept {
  assert(member_storage_ != nullptr);
  assert(member_id < member_storage_->size());
  return member_storage_->view(member_id);
}

std::string_view ZSet::score_text_view(std::uint32_t member_id) const noexcept {
  assert(member_storage_ != nullptr);
  assert(member_id < member_storage_->size());
  return member_storage_->score_text(member_id);
}

void ZSet::rebind_indexes() noexcept {
  members_.set_members(member_storage_.get());
  entries_.set_members(member_storage_.get());
}

void ZSet::move_last_member_into_slot(std::uint32_t removed_member_id) {
  const auto last_member_id =
      static_cast<std::uint32_t>(member_storage_->size() - 1);
  if (removed_member_id == last_member_id) {
    member_storage_->pop_back();
    return;
  }

  const auto moved_score = member_storage_->score(last_member_id);
  const auto removed_snapshot = member_storage_->snapshot(removed_member_id);
  member_storage_->copy_ref(removed_member_id, last_member_id);

  const bool replaced_score_entry =
      entries_.replace_member_id(moved_score, last_member_id, removed_member_id);
  assert(replaced_score_entry);
  if (!replaced_score_entry) {
    member_storage_->restore_snapshot(removed_member_id, removed_snapshot);
    return;
  }

  const bool moved_member = members_.move_member_id(last_member_id, removed_member_id);
  assert(moved_member);
  if (!moved_member) {
    const bool restored_score_entry =
        entries_.replace_member_id(moved_score, removed_member_id, last_member_id);
    assert(restored_score_entry);
    member_storage_->restore_snapshot(removed_member_id, removed_snapshot);
    (void)restored_score_entry;
    return;
  }
  member_storage_->pop_back();
}

int ZSet::add(double score, std::string_view member) {
  if (!std::isfinite(score)) {
    return 0;
  }

  auto* meta = members_.find(member);
  if (meta != nullptr) {
    const auto member_id = meta->member_id;
    const auto old_score = member_storage_->score(member_id);
    if (old_score == score) {
      return 0;
    }

    const bool removed =
        entries_.erase_one(ZSetScoreEntry{.score = old_score, .member_id = member_id});
    assert(removed);
    if (!removed) {
      return 0;
    }

    member_storage_->set_score(member_id, score);
    entries_.insert(ZSetScoreEntry{.score = score, .member_id = member_id});
    return 0;
  }

  const auto member_id = allocate_member_id(member, score);
  const auto view = member_view(member_id);
  members_.insert(view, ZSetMemberMeta{.member_id = member_id});
  entries_.insert(ZSetScoreEntry{.score = score, .member_id = member_id});

  return 1;
}

bool ZSet::remove(std::string_view member) {
  const auto* meta = members_.find(member);
  if (meta == nullptr) {
    return false;
  }

  const auto member_id = meta->member_id;
  const auto old_score = member_storage_->score(member_id);
  const bool removed =
      entries_.erase_one(ZSetScoreEntry{.score = old_score, .member_id = member_id});
  assert(removed);
  if (!removed) {
    return false;
  }

  const bool erased = members_.erase(member);
  assert(erased);
  move_last_member_into_slot(member_id);
  return erased;
}

std::optional<double> ZSet::score(std::string_view member) const {
  const auto* meta = members_.find(member);
  if (meta == nullptr) {
    return std::nullopt;
  }

  return member_storage_->score(meta->member_id);
}

std::optional<std::size_t> ZSet::rank(std::string_view member) const {
  const auto* meta = members_.find(member);
  if (meta == nullptr) {
    return std::nullopt;
  }

  return entries_.rank(ZSetScoreEntry{.score = member_storage_->score(meta->member_id),
                                      .member_id = meta->member_id});
}

std::optional<std::size_t> ZSet::reverse_rank(std::string_view member) const {
  const auto forward_rank = rank(member);
  if (!forward_rank) {
    return std::nullopt;
  }

  return size() - 1 - *forward_rank;
}

std::optional<ZSetRangeBounds> ZSet::range_bounds(long long start,
                                                  long long stop) const noexcept {
  const auto size = entries_.size();
  if (size == 0) {
    return std::nullopt;
  }

  start = normalize_index(start, size);
  stop = normalize_index(stop, size);

  if (start < 0) {
    start = 0;
  }
  if (stop < 0 || start > stop || start >= static_cast<long long>(size)) {
    return std::nullopt;
  }
  if (stop >= static_cast<long long>(size)) {
    stop = static_cast<long long>(size) - 1;
  }

  return ZSetRangeBounds{.first = static_cast<std::size_t>(start),
                         .count = static_cast<std::size_t>(stop - start + 1)};
}

std::vector<ZSetEntry> ZSet::range(long long start, long long stop) const {
  std::vector<ZSetEntry> out;
  const auto bounds = range_bounds(start, stop);
  if (!bounds) {
    return out;
  }

  out.reserve(bounds->count);
  auto append = [this, &out](double score, std::uint32_t member_id) {
    out.push_back(ZSetEntry{.member = member_view(member_id),
                            .score = score,
                            .score_text = score_text_view(member_id)});
  };
  entries_.for_range(bounds->first, bounds->count, append);
  return out;
}

std::vector<ZSetEntry> ZSet::reverse_range(long long start, long long stop) const {
  std::vector<ZSetEntry> out;
  const auto bounds = range_bounds(start, stop);
  if (!bounds) {
    return out;
  }

  out.reserve(bounds->count);
  auto append = [this, &out](double score, std::uint32_t member_id) {
    out.push_back(ZSetEntry{.member = member_view(member_id),
                            .score = score,
                            .score_text = score_text_view(member_id)});
  };
  entries_.for_reverse_range(bounds->first, bounds->count, append);
  return out;
}

bool ZSet::check_invariants() const {
  if (members_.size() != entries_.size()) {
    return false;
  }
  if (!entries_.validate()) {
    return false;
  }

  bool ok = true;
  members_.for_each([this, &ok](std::string_view member, const ZSetMemberMeta& meta) {
    if (!ok) {
      return;
    }
    ok = member_storage_ != nullptr &&
         meta.member_id < member_storage_->size() &&
         member == member_view(meta.member_id) &&
         entries_.contains(ZSetScoreEntry{.score = member_storage_->score(meta.member_id),
                                          .member_id = meta.member_id});
  });
  return ok;
}

ZSetMemoryStats ZSet::memory_stats() const noexcept {
  ZSetMemoryStats stats;
  stats.member_count = size();
  stats.rank_cache_mode = entries_.rank_cache_mode();
  if (member_storage_ != nullptr) {
    stats.member_storage_bytes = member_storage_->byte_size();
    stats.member_storage_allocated_bytes = member_storage_->allocated_bytes();
    stats.member_ref_capacity = member_storage_->ref_capacity();
    stats.score_string_cache_bytes = member_storage_->score_text_byte_size();
    stats.score_string_cache_ref_capacity =
        member_storage_->score_text_ref_capacity();
    stats.score_string_cache_allocated_bytes =
        member_storage_->score_text_allocated_bytes();
  }
  stats.member_index_capacity = members_.capacity();
  stats.member_index_member_slot_capacity = members_.member_slot_capacity();
  stats.member_index_tombstones = members_.tombstone_count();
  stats.member_index_allocated_bytes = members_.allocated_bytes();
  stats.score_entry_count = entries_.size();
  stats.score_block_count = entries_.block_count();
  stats.score_block_capacity_sum = entries_.block_capacity_sum();
  stats.rank_location_cache_allocated_bytes =
      entries_.location_cache_allocated_bytes();
  stats.score_index_allocated_bytes = entries_.allocated_bytes();
  stats.total_allocated_bytes = stats.member_storage_allocated_bytes +
                                stats.member_index_allocated_bytes +
                                stats.score_index_allocated_bytes;
  return stats;
}

void ZSet::compact(double member_index_density) {
  const auto old_entries = entries_.range(0, entries_.size());

  std::size_t member_bytes = 0;
  for (const auto& old_entry : old_entries) {
    member_bytes += member_view(old_entry.member_id).size();
  }

  auto new_storage =
      std::make_unique<ZSetMemberStorage>(options_.score_string_cache);
  new_storage->reserve(old_entries.size());
  new_storage->reserve_bytes(member_bytes);
  if (options_.score_string_cache) {
    std::size_t score_text_bytes = 0;
    for (const auto& old_entry : old_entries) {
      score_text_bytes += score_format::format(old_entry.score).size();
    }
    new_storage->reserve_score_text_bytes(score_text_bytes);
  }

  ZSetMemberIndex new_members(new_storage.get(), options_.member_index_growth);
  new_members.reserve_for_density(old_entries.size(), member_index_density);

  std::vector<ZSetScoreEntry> new_entries;
  new_entries.reserve(old_entries.size());

  for (const auto& old_entry : old_entries) {
    const auto view = member_view(old_entry.member_id);
    const auto new_id = new_storage->push_back(view, old_entry.score);
    new_members.insert_packed(view, ZSetMemberMeta{.member_id = new_id});
    new_entries.push_back(ZSetScoreEntry{.score = old_entry.score, .member_id = new_id});
  }

  ZSetScoreIndex new_score_index(new_storage.get(), options_.rank_cache_mode);
  new_score_index.assign_sorted(new_entries);

  member_storage_ = std::move(new_storage);
  members_ = std::move(new_members);
  entries_ = std::move(new_score_index);
  rebind_indexes();
  release_unused_heap_pages();
}

std::size_t ZSet::allocated_member_slots() const noexcept {
  return member_storage_ == nullptr ? 0 : member_storage_->size();
}

std::size_t ZSet::free_member_slots() const noexcept {
  return 0;
}

std::size_t ZSet::member_index_capacity() const noexcept {
  return members_.capacity();
}

std::size_t ZSet::member_index_tombstones() const noexcept {
  return members_.tombstone_count();
}

bool ZSet::should_compact_after_removal(std::size_t removed_count) const noexcept {
  if (removed_count < kMinRemovedForCompactionCheck || empty()) {
    return false;
  }

  const auto allocated = allocated_member_slots();
  const auto free = free_member_slots();
  return free >= kMinFreeSlotsForCompaction &&
         free * kCompactionFreeSlotsPerAllocatedSlot >= allocated;
}

bool ZSet::cleanup_member_index_after_removal_if_needed(std::size_t removed_count) {
  return members_.cleanup_after_removal_if_needed(removed_count);
}

bool ZSet::rehash_member_index_same_capacity() {
  return members_.rehash_same_capacity();
}

bool ZSet::compact_after_removal_if_needed(std::size_t removed_count) {
  if (!should_compact_after_removal(removed_count)) {
    return false;
  }

  compact();
  return true;
}

Store::Store(StoreOptions options) : options_(options) {}

ZSet* Store::find_zset(std::string_view key) noexcept {
  if (inline_zset_.has_value() && std::string_view(inline_key_) == key) {
    return &*inline_zset_;
  }
  if (overflow_zsets_.empty()) {
    return nullptr;
  }
  return overflow_zsets_.find(std::string(key));
}

const ZSet* Store::find_zset(std::string_view key) const noexcept {
  if (inline_zset_.has_value() && std::string_view(inline_key_) == key) {
    return &*inline_zset_;
  }
  if (overflow_zsets_.empty()) {
    return nullptr;
  }
  return overflow_zsets_.find(std::string(key));
}

ZSet& Store::get_or_create_zset(std::string_view key) {
  if (inline_zset_.has_value() && std::string_view(inline_key_) == key) {
    return *inline_zset_;
  }

  if (!overflow_zsets_.empty()) {
    auto* overflow = overflow_zsets_.find(std::string(key));
    if (overflow != nullptr) {
      return *overflow;
    }
  }

  if (!inline_zset_.has_value() && overflow_zsets_.empty()) {
    inline_key_.assign(key);
    inline_zset_.emplace(ZSetOptions{
        .rank_cache_mode = options_.rank_cache_mode,
        .score_string_cache = options_.score_string_cache,
        .member_index_growth = options_.member_index_growth,
    });
    return *inline_zset_;
  }

  auto [zset, inserted] = overflow_zsets_.try_emplace(
      std::string(key),
      ZSetOptions{
          .rank_cache_mode = options_.rank_cache_mode,
          .score_string_cache = options_.score_string_cache,
          .member_index_growth = options_.member_index_growth,
      });
  (void)inserted;
  return *zset;
}

void Store::erase_if_empty(std::string_view key, const ZSet& zset) {
  if (!zset.empty()) {
    return;
  }

  if (inline_zset_.has_value() && std::string_view(inline_key_) == key &&
      &*inline_zset_ == &zset) {
    inline_zset_.reset();
    inline_key_.clear();
    return;
  }

  overflow_zsets_.erase(std::string(key));
}

long long Store::zadd(std::string_view key, double score, std::string_view member) {
  return get_or_create_zset(key).add(score, member);
}

long long Store::zrem(std::string_view key, std::span<const std::string_view> members) {
  auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }

  long long removed = 0;
  for (const auto& member : members) {
    removed += zset->remove(member) ? 1 : 0;
  }

  if (!zset->compact_after_removal_if_needed(static_cast<std::size_t>(removed))) {
    (void)zset->cleanup_member_index_after_removal_if_needed(
        static_cast<std::size_t>(removed));
  }
  erase_if_empty(key, *zset);

  return removed;
}

long long Store::zcard(std::string_view key) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }

  return static_cast<long long>(zset->size());
}

std::optional<double> Store::zscore(std::string_view key,
                                    std::string_view member) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return std::nullopt;
  }

  return zset->score(member);
}

std::optional<std::size_t> Store::zrank(std::string_view key,
                                        std::string_view member) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return std::nullopt;
  }

  return zset->rank(member);
}

std::optional<std::size_t> Store::zrevrank(std::string_view key,
                                          std::string_view member) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return std::nullopt;
  }

  return zset->reverse_rank(member);
}

std::vector<ZSetEntry> Store::zrange(std::string_view key,
                                     long long start,
                                     long long stop) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return {};
  }

  return zset->range(start, stop);
}

std::vector<ZSetEntry> Store::zrevrange(std::string_view key,
                                        long long start,
                                        long long stop) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return {};
  }

  return zset->reverse_range(start, stop);
}

std::size_t Store::zrange_size(std::string_view key,
                               long long start,
                               long long stop) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }

  const auto bounds = zset->range_bounds(start, stop);
  return bounds ? bounds->count : 0;
}

std::size_t Store::zrevrange_size(std::string_view key,
                                  long long start,
                                  long long stop) const {
  return zrange_size(key, start, stop);
}

std::optional<ZSetMemoryStats> Store::zset_memory_stats(std::string_view key) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return std::nullopt;
  }

  return zset->memory_stats();
}

std::optional<std::size_t> Store::optimize(std::string_view key,
                                           double member_index_density) {
  auto* zset = find_zset(key);
  if (zset == nullptr) {
    return std::nullopt;
  }

  const auto before = zset->memory_stats().total_allocated_bytes;
  zset->compact(member_index_density);
  const auto after = zset->memory_stats().total_allocated_bytes;
  return before > after ? before - after : 0;
}

void ZSet::save(snapshot::Writer& writer, bool with_accelerator) const {
  writer.u8(static_cast<std::uint8_t>(options_.rank_cache_mode));
  writer.u8(options_.score_string_cache ? 1 : 0);
  writer.f64(options_.member_index_growth);

  const auto member_count = static_cast<std::uint32_t>(member_storage_->size());
  writer.u64(member_count);

  // Canonical layer: (score, member bytes) in member-id order. This alone is
  // enough to reconstruct the zset.
  for (std::uint32_t id = 0; id < member_count; ++id) {
    writer.f64(member_storage_->score(id));
    writer.str(member_storage_->view(id));
  }

  // Accelerator layer (optional): the member index dumped verbatim, then member
  // ids in ascending score order (so the score index rebuilds without sorting).
  writer.u8(with_accelerator ? 1 : 0);
  if (with_accelerator) {
    members_.write_accelerator(writer);
    const auto ordered = entries_.range(0, entries_.size());
    for (const auto& entry : ordered) {
      writer.u32(entry.member_id);
    }
  }
}

ZSet ZSet::load(snapshot::Reader& reader, bool use_accelerator) {
  ZSetOptions options;
  options.rank_cache_mode = static_cast<RankCacheMode>(reader.u8());
  options.score_string_cache = reader.u8() != 0;
  options.member_index_growth = reader.f64();

  ZSet zset(options);
  const auto member_count = static_cast<std::uint32_t>(reader.u64());
  zset.member_storage_->reserve(member_count);

  // Canonical rebuild: push_back assigns member ids 0..N-1 in the same order
  // they were written, so accelerator member ids stay valid. Dead arena bytes
  // are naturally dropped.
  for (std::uint32_t id = 0; id < member_count; ++id) {
    const double score = reader.f64();
    const auto member = reader.str();
    (void)zset.member_storage_->push_back(member, score);
  }

  const bool accelerator_present = reader.u8() != 0;
  if (accelerator_present && use_accelerator) {
    zset.members_.read_accelerator(reader, zset.member_storage_.get());
    if (zset.members_.size() != member_count) {
      throw snapshot::snapshot_error("snapshot member index size mismatch");
    }
    std::vector<ZSetScoreEntry> ordered;
    ordered.reserve(member_count);
    for (std::uint32_t i = 0; i < member_count; ++i) {
      const auto id = reader.u32();
      if (id >= member_count) {
        throw snapshot::snapshot_error("snapshot score order id out of range");
      }
      ordered.push_back(
          ZSetScoreEntry{.score = zset.member_storage_->score(id), .member_id = id});
    }
    zset.entries_.assign_sorted(ordered);
  } else {
    // Slow path: rebuild both indexes from the canonical members. The score
    // index is built with insert() so that equal scores order by member bytes
    // exactly as the live structure does. Any present-but-unused accelerator
    // bytes are bounded by the instruction's operand length and simply ignored.
    zset.members_.reserve_for_density(member_count, kDefaultMemberIndexDensity);
    for (std::uint32_t id = 0; id < member_count; ++id) {
      zset.members_.insert_packed(zset.member_storage_->view(id),
                                  ZSetMemberMeta{.member_id = id});
      zset.entries_.insert(
          ZSetScoreEntry{.score = zset.member_storage_->score(id), .member_id = id});
    }
  }

  zset.rebind_indexes();
  return zset;
}

void Store::save(std::ostream& out) const {
  // File header, then one section per data family. Only ZSET exists today.
  std::string header;
  snapshot::Writer writer(header);
  writer.bytes(snapshot::kMagic, sizeof(snapshot::kMagic));
  writer.u32(snapshot::kFormatVersion);
  writer.u32(0);  // file flags (reserved)
  writer.u32(1);  // section count
  // ZSET section header: family + accelerator version.
  writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Zset));
  writer.u32(snapshot::kZsetAcceleratorVersion);
  out.write(header.data(), static_cast<std::streamsize>(header.size()));

  // ZSET section body: a stream of OP_ZSET instructions, then OP_END.
  std::string operands;
  auto emit_zset = [&out, &operands](std::string_view key, const ZSet& zset) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    zset.save(operand_writer, /*with_accelerator=*/true);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::ZsetOpcode::Zset));
    instruction_writer.u64(operands.size());
    instruction_writer.u64(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };

  if (inline_zset_.has_value()) {
    emit_zset(inline_key_, *inline_zset_);
  }
  overflow_zsets_.for_each([&emit_zset](const std::pair<std::string, ZSet>& entry) {
    emit_zset(entry.first, entry.second);
  });

  const char end = static_cast<char>(snapshot::kOpEnd);
  out.write(&end, 1);

  if (!out) {
    throw snapshot::snapshot_error("snapshot write failed");
  }
}

SnapshotLoadStats Store::load(std::istream& in) {
  inline_zset_.reset();
  inline_key_.clear();
  overflow_zsets_.clear();

  // A corrupt length field must fail cleanly, not attempt a wild allocation.
  constexpr std::uint64_t kMaxOperandBytes = std::uint64_t{1} << 40;
  auto read_exact = [&in](std::uint64_t size) {
    if (size > kMaxOperandBytes) {
      throw snapshot::snapshot_error("snapshot length implausible");
    }
    std::string buffer(size, '\0');
    in.read(buffer.data(), static_cast<std::streamsize>(size));
    if (static_cast<std::uint64_t>(in.gcount()) != size) {
      throw snapshot::snapshot_error("snapshot truncated");
    }
    return buffer;
  };

  try {
    const auto header = read_exact(sizeof(snapshot::kMagic) + 4 + 4 + 4);
    snapshot::Reader header_reader(header);
    if (header_reader.bytes(sizeof(snapshot::kMagic)) !=
        std::string_view(snapshot::kMagic, sizeof(snapshot::kMagic))) {
      throw snapshot::snapshot_error("not a Goblin Core snapshot");
    }
    if (header_reader.u32() != snapshot::kFormatVersion) {
      throw snapshot::snapshot_error("unsupported snapshot version");
    }
    (void)header_reader.u32();  // file flags (reserved)
    const auto section_count = header_reader.u32();

    SnapshotLoadStats stats;
    for (std::uint32_t s = 0; s < section_count; ++s) {
      const auto section_header = read_exact(4 + 4);
      snapshot::Reader section_reader(section_header);
      const auto section_type = section_reader.u32();
      const auto section_version = section_reader.u32();

      const bool is_zset =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::Zset);
      const bool use_accelerator =
          is_zset && section_version == snapshot::kZsetAcceleratorVersion;
      if (is_zset) {
        stats.used_accelerator = use_accelerator;
      }

      // Interpret the section's instruction stream until OP_END.
      for (;;) {
        const auto opcode = static_cast<std::uint8_t>(read_exact(1)[0]);
        if (opcode == snapshot::kOpEnd) {
          break;
        }
        const auto frame = read_exact(16);
        snapshot::Reader frame_reader(frame);
        const auto operand_size = frame_reader.u64();
        const auto expected_checksum = frame_reader.u64();
        const auto operands = read_exact(operand_size);
        if (snapshot::checksum(operands) != expected_checksum) {
          throw snapshot::snapshot_error("snapshot checksum mismatch");
        }

        const bool is_zset_op =
            is_zset && opcode == static_cast<std::uint8_t>(snapshot::ZsetOpcode::Zset);
        if (!is_zset_op) {
          continue;  // unknown opcode or unknown section: skip (already consumed)
        }
        snapshot::Reader reader(operands);
        auto key = std::string(reader.str());
        ZSet zset = ZSet::load(reader, use_accelerator);
        stats.members += zset.size();
        place_loaded_zset(std::move(key), std::move(zset));
        ++stats.keys;
      }
    }
    return stats;
  } catch (...) {
    inline_zset_.reset();
    inline_key_.clear();
    overflow_zsets_.clear();
    throw;
  }
}

void Store::place_loaded_zset(std::string key, ZSet&& zset) {
  if (!inline_zset_.has_value() && overflow_zsets_.empty()) {
    inline_key_ = std::move(key);
    inline_zset_.emplace(std::move(zset));
  } else {
    overflow_zsets_.try_emplace(std::move(key), std::move(zset));
  }
}

StoreMemoryStats Store::memory_stats() const noexcept {
  StoreMemoryStats stats;
  stats.inline_zset_count = inline_zset_.has_value() ? 1 : 0;
  stats.overflow_zset_count = overflow_zsets_.size();
  stats.overflow_zset_capacity = overflow_zsets_.capacity();
  stats.overflow_table_allocated_bytes = overflow_zsets_.allocated_bytes();
  if (inline_zset_.has_value()) {
    stats.inline_zset_allocated_bytes =
        inline_zset_->memory_stats().total_allocated_bytes;
  }
  overflow_zsets_.for_each([&stats](const auto& entry) {
    stats.overflow_zset_allocated_bytes +=
        entry.second.memory_stats().total_allocated_bytes;
  });
  stats.total_allocated_bytes = stats.overflow_table_allocated_bytes +
                                stats.inline_zset_allocated_bytes +
                                stats.overflow_zset_allocated_bytes;
  return stats;
}

std::string format_score(double score) {
  return score_format::format(score);
}

}  // namespace goblin::core
