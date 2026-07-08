#include "goblin/core/store.hpp"

#include "goblin/core/rdb.hpp"

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
#include <cerrno>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace goblin::core {
namespace {

// Arena auto-compaction floor: rebuild only once at least this many member
// bytes are dead (and dead has caught up to live), so small sets never churn.
constexpr std::size_t kMemberArenaAutoCompactDeadFloor = std::size_t{1} << 20;

// Parse a base-10 signed 64-bit integer, rejecting trailing junk (for HINCRBY).
[[nodiscard]] std::optional<long long> parse_i64(std::string_view text) {
  long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

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

// Minimal std::ostream backing that write()s to a raw fd, so the background-save
// child can fsync the descriptor. Store::save writes in large chunks (one per
// zset), so there are few syscalls; no user-space buffering is needed.
class FdOutputStreambuf : public std::streambuf {
 public:
  explicit FdOutputStreambuf(int fd) noexcept : fd_(fd) {}

 protected:
  std::streamsize xsputn(const char* data, std::streamsize count) override {
    std::streamsize total = 0;
    while (total < count) {
      const auto written =
          ::write(fd_, data + total, static_cast<std::size_t>(count - total));
      if (written < 0) {
        if (errno == EINTR) continue;
        return total;  // short write -> the ostream fails, caller detects it
      }
      total += written;
    }
    return total;
  }

  int_type overflow(int_type ch) override {
    if (ch == traits_type::eof()) return ch;
    const char byte = static_cast<char>(ch);
    return xsputn(&byte, 1) == 1 ? ch : traits_type::eof();
  }

 private:
  int fd_;
};

}  // namespace

ZSetMemberLayer::ZSetMemberLayer(bool score_string_cache,
                                 std::size_t member_chunk_bytes,
                                 double member_index_growth)
    : storage(std::make_shared<ZSetMemberStorage>(score_string_cache,
                                                  member_chunk_bytes,
                                                  member_index_growth)),
      members(storage.get(), member_index_growth) {}

std::shared_ptr<ZSetMemberLayer> ZSetMemberLayer::clone(
    double member_index_growth) const {
  auto cloned = std::make_shared<ZSetMemberLayer>(
      storage->score_string_cache_enabled(), storage->chunk_bytes(),
      member_index_growth);

  const auto member_count = storage->size();
  if (member_count == 0) {
    return cloned;
  }

  cloned->storage->reserve(member_count);
  std::size_t member_bytes = 0;
  std::size_t score_text_bytes = 0;
  for (std::uint32_t id = 0; id < member_count; ++id) {
    const auto view = storage->view(id);
    member_bytes += view.size();
    if (storage->score_string_cache_enabled()) {
      score_text_bytes += storage->score_text(id).size();
    }
  }
  cloned->storage->reserve_bytes(member_bytes);
  if (storage->score_string_cache_enabled()) {
    cloned->storage->reserve_score_text_bytes(score_text_bytes);
  }

  cloned->members.reserve_for_density(member_count, kDefaultMemberIndexDensity);
  for (std::uint32_t id = 0; id < member_count; ++id) {
    const auto view = storage->view(id);
    const auto new_id =
        cloned->storage->push_back(view, storage->score(id));
    cloned->members.insert_packed(view, ZSetMemberMeta{.member_id = new_id});
  }
  return cloned;
}

std::shared_ptr<ZSetMemberLayer> ZSetMemberLayer::clone_shallow(
    double member_index_growth) const {
  auto cloned = std::make_shared<ZSetMemberLayer>(
      storage->score_string_cache_enabled(), storage->chunk_bytes(),
      member_index_growth);
  cloned->storage =
      std::make_shared<ZSetMemberStorage>(storage->clone_shallow());
  cloned->members = members.clone_rebound(cloned->storage.get());
  return cloned;
}

// Build a fresh empty representation into rep_: a listpack when the option is on,
// else the full arena-shaped structure. Used by construction and by move (to leave
// the moved-from source valid).
void ZSet::init_empty() {
  if (options_->listpack_max_entries == 0) {
    auto member_layer = std::make_shared<ZSetMemberLayer>(
        options_->score_string_cache, options_->member_chunk_bytes,
        options_->member_index_growth);
    auto score_index = std::make_shared<ZSetScoreIndex>(
        member_layer->storage.get(), options_->rank_cache_mode);
    rep_ = FullState{std::move(member_layer), std::move(score_index)};
    rebind_indexes();
  } else {
    ZSetListpack lp;
    lp.set_max_entries(options_->listpack_max_entries);
    rep_ = std::move(lp);
  }
}

namespace {
// Shared default options for standalone, default-constructed zsets (the store
// points its own zsets at its owned options instead).
const ZSetOptions kDefaultZSetOptions{};
}  // namespace

ZSet::ZSet() : options_(&kDefaultZSetOptions) { init_empty(); }

ZSet::ZSet(const ZSetOptions* options) : options_(options) { init_empty(); }

ZSet::ZSet(ZSet&& other) noexcept
    : rep_(std::move(other.rep_)), options_(other.options_) {
  other.init_empty();  // leave the moved-from source a valid empty zset
  if (!is_small()) {
    rebind_indexes();  // re-point the indexes at the moved member storage
  }
}

ZSet& ZSet::operator=(ZSet&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  rep_ = std::move(other.rep_);
  options_ = other.options_;
  other.init_empty();
  if (!is_small()) {
    rebind_indexes();
  }
  return *this;
}

ZSetMemberStorage* ZSet::member_storage() noexcept {
  return full().member_layer->storage.get();
}

const ZSetMemberStorage* ZSet::member_storage() const noexcept {
  return full().member_layer->storage.get();
}

ZSetMemberIndex& ZSet::members() noexcept { return full().member_layer->members; }

const ZSetMemberIndex& ZSet::members() const noexcept {
  return full().member_layer->members;
}

ZSetScoreIndex& ZSet::entries() noexcept { return *full().score_index; }

const ZSetScoreIndex& ZSet::entries() const noexcept {
  return *full().score_index;
}

void ZSet::ensure_unique_mutable_state(WriteKind kind) {
  auto& fs = full();
  if (fs.member_layer.use_count() > 1) {
    fs.member_layer = fs.member_layer->clone_shallow(options_->member_index_growth);
  }
  if (kind == WriteKind::Structural) {
    member_storage()->ensure_unique_arena();
  }
  if (fs.score_index.use_count() > 1) {
    auto cloned = std::make_shared<ZSetScoreIndex>(member_storage(),
                                                   options_->rank_cache_mode);
    cloned->copy_blocks_from(entries());
    fs.score_index = std::move(cloned);
  }
  rebind_indexes();
}

void ZSet::adopt_shared_member_layer_from(const ZSet& source) {
  // Adopting a shared full member layer makes this a full zset.
  auto member_layer = source.full().member_layer;
  auto copied = std::make_shared<ZSetScoreIndex>(member_layer->storage.get(),
                                                 options_->rank_cache_mode);
  copied->copy_blocks_from(source.entries());
  rep_ = FullState{std::move(member_layer), std::move(copied)};
  rebind_indexes();
}

bool ZSet::empty() const noexcept {
  if (const auto* lp = small_ptr()) {
    return lp->empty();
  }
  return entries().empty();
}

std::size_t ZSet::size() const noexcept {
  if (const auto* lp = small_ptr()) {
    return lp->size();
  }
  return entries().size();
}

std::size_t ZSet::block_count() const noexcept {
  return is_small() ? 0 : entries().block_count();
}

std::uint32_t ZSet::allocate_member_id(std::string_view member, double score) {
  return member_storage()->push_back(member, score);
}

std::string_view ZSet::member_view(std::uint32_t member_id) const noexcept {
  assert(member_storage() != nullptr);
  assert(member_id < member_storage()->size());
  return member_storage()->view(member_id);
}

std::string_view ZSet::score_text_view(std::uint32_t member_id) const noexcept {
  assert(member_storage() != nullptr);
  assert(member_id < member_storage()->size());
  return member_storage()->score_text(member_id);
}

void ZSet::rebind_indexes() noexcept {
  members().set_members(member_storage());
  entries().set_members(member_storage());
}

// Convert the listpack blob into the full arena-shaped structure (member layer +
// swiss index + score index), replaying its entries in sorted order.
void ZSet::ensure_full() {
  auto* small = small_ptr();
  if (small == nullptr) {
    return;
  }
  ZSetListpack lp = std::move(*small);
  auto member_layer = std::make_shared<ZSetMemberLayer>(
      options_->score_string_cache, options_->member_chunk_bytes,
      options_->member_index_growth);
  auto score_index = std::make_shared<ZSetScoreIndex>(
      member_layer->storage.get(), options_->rank_cache_mode);
  rep_ = FullState{std::move(member_layer), std::move(score_index)};
  rebind_indexes();
  lp.for_each([this](double score, std::string_view member) {
    const auto member_id = allocate_member_id(member, score);
    members().insert(member_view(member_id),
                     ZSetMemberMeta{.member_id = member_id});
    entries().insert(ZSetScoreEntry{.score = score, .member_id = member_id});
  });
}

// A ZADD whose score falls outside the zset's current score width auto-promotes
// it (i16->i32->f64) inside the member storage, which rebuilds the score array
// O(n). DANGER: when the zset is copy-on-write shared (see
// adopt_shared_member_layer_from), ensure_unique_mutable_state below already forks
// the SoA O(n) -- so a *promoting* ZADD on a large shared set pays the fork plus
// the width rebuild (2*O(n) and transient extra memory), a latency/memory spike a
// single fractional or out-of-range score can trigger. Widening is one-way;
// GOBLIN.OPTIMIZE reclaims it by scanning and demoting to the narrowest fit.
int ZSet::add(double score, std::string_view member) {
  if (!std::isfinite(score)) {
    return 0;
  }

  if (auto* lp = small_ptr()) {
    const bool existed = lp->score(member).has_value();
    const auto result = lp->add(score, member);
    if (!result.needs_full) {
      return (result.changed && !existed) ? 1 : 0;
    }
    ensure_full();  // outgrew the listpack; fall through to the full add path
  }

  auto* meta = members().find(member);
  if (meta != nullptr) {
    const auto member_id = meta->member_id;
    const auto old_score = member_storage()->score(member_id);
    if (old_score == score) {
      return 0;
    }

    ensure_unique_mutable_state(WriteKind::ScoreUpdate);

    const bool removed =
        entries().erase_one(ZSetScoreEntry{.score = old_score, .member_id = member_id});
    assert(removed);
    if (!removed) {
      return 0;
    }

    member_storage()->set_score(member_id, score);
    entries().insert(ZSetScoreEntry{.score = score, .member_id = member_id});
    return 0;
  }

  ensure_unique_mutable_state(WriteKind::Structural);

  const auto member_id = allocate_member_id(member, score);
  const auto view = member_view(member_id);
  members().insert(view, ZSetMemberMeta{.member_id = member_id});
  entries().insert(ZSetScoreEntry{.score = score, .member_id = member_id});

  return 1;
}

bool ZSet::remove(std::string_view member) {
  if (auto* lp = small_ptr()) {
    return lp->remove(member);
  }
  const auto slot = members().find_slot(member);
  if (!slot) {
    return false;
  }

  ensure_unique_mutable_state(WriteKind::Structural);

  const auto member_id = members().member_id_at(*slot);
  const auto old_score = member_storage()->score(member_id);
  const auto last_member_id =
      static_cast<std::uint32_t>(member_storage()->size() - 1);

  if (member_id == last_member_id) {
    const bool removed =
        entries().erase_one(ZSetScoreEntry{.score = old_score, .member_id = member_id});
    assert(removed);
    if (!removed) {
      return false;
    }

    const bool erased = members().erase_at_index(*slot);
    assert(erased);
    member_storage()->orphan(member_id);
    member_storage()->pop_back();
    return erased;
  }

  const ZSetScoreEntry removed_entry{.score = old_score, .member_id = member_id};
  const auto last_score = member_storage()->score(last_member_id);
  const ZSetScoreEntry last_entry{.score = last_score, .member_id = last_member_id};
  const auto last_slot = members().find_slot(member_view(last_member_id));
  const auto removed_snapshot = member_storage()->snapshot(member_id);

  const auto removed_location = entries().find_entry_location(removed_entry);
  const auto last_location = entries().find_entry_location(last_entry);
  if (!removed_location || !last_location) {
    return false;
  }

  const bool erased_block_was_singleton =
      entries().block_was_singleton(removed_location->first);

  member_storage()->orphan(member_id);
  member_storage()->copy_ref(member_id, last_member_id);

  const bool score_updated = entries().erase_removed_and_retarget_last(
      *removed_location, *last_location, erased_block_was_singleton, last_entry,
      member_id);
  assert(score_updated);
  if (!score_updated) {
    member_storage()->restore_snapshot(member_id, removed_snapshot);
    return false;
  }

  const bool erased = members().erase_at_index(*slot);
  assert(erased);
  (void)erased;

  if (!last_slot) {
    member_storage()->restore_snapshot(member_id, removed_snapshot);
    return false;
  }

  const bool moved_member =
      members().move_member_id_at_slot(*last_slot, last_member_id, member_id);
  assert(moved_member);
  if (!moved_member) {
    member_storage()->restore_snapshot(member_id, removed_snapshot);
    return false;
  }

  member_storage()->pop_back();
  return erased;
}

std::optional<double> ZSet::score(std::string_view member) const {
  if (const auto* lp = small_ptr()) {
    return lp->score(member);
  }
  const auto* meta = members().find(member);
  if (meta == nullptr) {
    return std::nullopt;
  }

  return member_storage()->score(meta->member_id);
}

std::optional<std::size_t> ZSet::rank(std::string_view member) const {
  if (const auto* lp = small_ptr()) {
    return lp->rank(member);
  }
  const auto* meta = members().find(member);
  if (meta == nullptr) {
    return std::nullopt;
  }

  return entries().rank(ZSetScoreEntry{.score = member_storage()->score(meta->member_id),
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
  const auto size = is_small() ? small_ptr()->size() : entries().size();
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
  for_position_range(*bounds, false,
                     [&out](std::string_view member, double score,
                            std::string_view score_text) {
                       out.push_back(ZSetEntry{.member = member,
                                               .score = score,
                                               .score_text = score_text});
                     });
  return out;
}

std::vector<ZSetEntry> ZSet::reverse_range(long long start, long long stop) const {
  std::vector<ZSetEntry> out;
  const auto bounds = range_bounds(start, stop);
  if (!bounds) {
    return out;
  }

  out.reserve(bounds->count);
  for_position_range(*bounds, true,
                     [&out](std::string_view member, double score,
                            std::string_view score_text) {
                       out.push_back(ZSetEntry{.member = member,
                                               .score = score,
                                               .score_text = score_text});
                     });
  return out;
}

bool ZSet::check_invariants() const {
  if (is_small()) {
    return true;
  }
  if (members().size() != entries().size()) {
    return false;
  }
  if (!entries().validate()) {
    return false;
  }

  bool ok = true;
  members().for_each([this, &ok](std::string_view member, const ZSetMemberMeta& meta) {
    if (!ok) {
      return;
    }
    ok = member_storage() != nullptr &&
         meta.member_id < member_storage()->size() &&
         member == member_view(meta.member_id) &&
         entries().contains(ZSetScoreEntry{.score = member_storage()->score(meta.member_id),
                                          .member_id = meta.member_id});
  });
  return ok;
}

ZSetMemoryStats ZSet::memory_stats() const noexcept {
  ZSetMemoryStats stats;
  stats.member_count = size();
  if (const auto* lp = small_ptr()) {
    stats.rank_cache_mode = options_->rank_cache_mode;
    stats.score_width = lp->score_width();
    stats.member_storage_bytes = lp->blob_bytes();
    stats.member_storage_allocated_bytes = lp->allocated_bytes();
    stats.member_layer_share_count = 1;
    stats.score_index_share_count = 1;
    stats.total_allocated_bytes = lp->allocated_bytes();
    return stats;
  }
  stats.rank_cache_mode = entries().rank_cache_mode();
  const auto layer_shares = full().member_layer.use_count();
  stats.member_layer_share_count = layer_shares;
  if (member_storage() != nullptr) {
    stats.score_width = member_storage()->score_width();
    stats.member_storage_bytes = member_storage()->byte_size();
    stats.member_storage_allocated_bytes =
        member_storage()->allocated_bytes() / layer_shares;
    stats.member_ref_capacity = member_storage()->ref_capacity();
    stats.score_string_cache_bytes = member_storage()->score_text_byte_size();
    stats.score_string_cache_ref_capacity =
        member_storage()->score_text_ref_capacity();
    stats.score_string_cache_allocated_bytes =
        member_storage()->score_text_allocated_bytes() / layer_shares;
  }
  stats.member_index_capacity = members().capacity();
  stats.member_index_member_slot_capacity = members().member_slot_capacity();
  stats.member_index_tombstones = members().tombstone_count();
  stats.member_index_allocated_bytes = members().allocated_bytes() / layer_shares;
  const auto score_shares = full().score_index.use_count();
  stats.score_index_share_count = score_shares;
  stats.score_entry_count = entries().size();
  stats.score_block_count = entries().block_count();
  stats.score_block_capacity_sum = entries().block_capacity_sum();
  stats.rank_location_cache_allocated_bytes =
      entries().location_cache_allocated_bytes() / score_shares;
  stats.score_index_allocated_bytes = entries().allocated_bytes() / score_shares;
  stats.total_allocated_bytes = stats.member_storage_allocated_bytes +
                                stats.member_index_allocated_bytes +
                                stats.score_index_allocated_bytes;
  return stats;
}

void ZSet::compact(double member_index_density) {
  if (auto* lp = small_ptr()) {
    lp->optimize();  // re-derive the narrowest score width (demote)
    return;          // the blob is already structurally compact
  }
  const auto old_entries = entries().range(0, entries().size());

  std::size_t member_bytes = 0;
  for (const auto& old_entry : old_entries) {
    member_bytes += member_view(old_entry.member_id).size();
  }

  auto new_layer = std::make_shared<ZSetMemberLayer>(
      options_->score_string_cache, options_->member_chunk_bytes,
      options_->member_index_growth);
  new_layer->storage->reserve(old_entries.size());
  new_layer->storage->reserve_bytes(member_bytes);
  if (options_->score_string_cache) {
    std::size_t score_text_bytes = 0;
    for (const auto& old_entry : old_entries) {
      score_text_bytes += score_format::format(old_entry.score).size();
    }
    new_layer->storage->reserve_score_text_bytes(score_text_bytes);
  }

  new_layer->members.reserve_for_density(old_entries.size(), member_index_density);

  std::vector<ZSetScoreEntry> new_entries;
  new_entries.reserve(old_entries.size());

  for (const auto& old_entry : old_entries) {
    const auto view = member_view(old_entry.member_id);
    const auto new_id = new_layer->storage->push_back(view, old_entry.score);
    new_layer->members.insert_packed(view, ZSetMemberMeta{.member_id = new_id});
    new_entries.push_back(ZSetScoreEntry{.score = old_entry.score, .member_id = new_id});
  }

  ZSetScoreIndex new_score_index(new_layer->storage.get(), options_->rank_cache_mode);
  new_score_index.assign_sorted(new_entries);

  rep_ = FullState{
      std::move(new_layer),
      std::make_shared<ZSetScoreIndex>(std::move(new_score_index))};
  rebind_indexes();
  release_unused_heap_pages();
}

std::size_t ZSet::allocated_member_slots() const noexcept {
  return is_small() ? 0 : member_storage()->size();
}

std::size_t ZSet::free_member_slots() const noexcept {
  return 0;
}

std::size_t ZSet::member_index_capacity() const noexcept {
  return is_small() ? 0 : members().capacity();
}

std::size_t ZSet::member_index_tombstones() const noexcept {
  return is_small() ? 0 : members().tombstone_count();
}

bool ZSet::should_compact_after_removal(std::size_t removed_count) const noexcept {
  if (is_small() || removed_count == 0 || empty()) {
    return false;
  }

  // Member ids are dense (swap-remove), so there are no free index slots to
  // reclaim -- the fragmentation lives in the member-bytes arena, where a
  // removal orphans the member's bytes until a rebuild. Compact once the dead
  // bytes exceed the live bytes (bounding the arena at ~2x live) past a floor.
  const auto dead = member_storage()->dead_bytes();
  return dead >= kMemberArenaAutoCompactDeadFloor &&
         dead >= member_storage()->live_bytes();
}

bool ZSet::cleanup_member_index_after_removal_if_needed(std::size_t removed_count) {
  return is_small() ? false
                : members().cleanup_after_removal_if_needed(removed_count);
}

bool ZSet::rehash_member_index_same_capacity() {
  return is_small() ? false : members().rehash_same_capacity();
}

bool ZSet::compact_after_removal_if_needed(std::size_t removed_count) {
  if (!should_compact_after_removal(removed_count)) {
    return false;
  }

  // Id-stable arena reclaim: repack live member bytes into a fresh page-aligned
  // arena (only offsets change -- no swiss/score index rebuild, unlike the full
  // compact() used by GOBLIN.OPTIMIZE), then hand the freed pages back to the OS.
  // The arena is unique here (the preceding ZREM forked it). The swiss index's
  // own tombstones are reclaimed separately by the member-index cleanup.
  member_storage()->compact();
  release_unused_heap_pages();
  return true;
}

Store::Store(StoreOptions options) : options_(options) {}

ZSet* Store::find_inline_zset(std::string_view key) noexcept {
  const auto* index = inline_zset_index_.find(key);
  if (index == nullptr || *index >= inline_zsets_.size()) {
    return nullptr;
  }
  return &inline_zsets_[*index].zset;
}

const ZSet* Store::find_inline_zset(std::string_view key) const noexcept {
  const auto* index = inline_zset_index_.find(key);
  if (index == nullptr || *index >= inline_zsets_.size()) {
    return nullptr;
  }
  return &inline_zsets_[*index].zset;
}

bool Store::inline_zset_slots_full() const noexcept {
  return inline_zsets_.size() >= options_.inline_zset_limit;
}

const ZSetOptions* Store::zset_options() {
  if (!zset_options_ready_) {
    zset_options_ = ZSetOptions{
        .rank_cache_mode = options_.rank_cache_mode,
        .score_string_cache = options_.score_string_cache,
        .member_index_growth = options_.member_index_growth,
        .member_chunk_bytes = options_.zset_chunk_bytes,
        // The score-string cache preserves exact input text, which the numeric
        // listpack can't -- so enabling it keeps zsets in the full structure.
        .listpack_max_entries = options_.score_string_cache
                                    ? std::size_t{0}
                                    : options_.zset_listpack_max_entries,
    };
    zset_options_ready_ = true;
  }
  return &zset_options_;
}

ZSet& Store::emplace_inline_zset(std::string_view key) {
  const auto index = inline_zsets_.size();
  inline_zsets_.push_back(InlineZsetSlot{
      .key = std::string(key),
      .zset = ZSet(zset_options()),
  });
  auto [slot, inserted] = inline_zset_index_.try_emplace(inline_zsets_.back().key, index);
  (void)inserted;
  (void)slot;
  return inline_zsets_.back().zset;
}

void Store::erase_inline_zset_if(std::string_view key,
                                 const ZSet& zset) noexcept {
  const auto* index = inline_zset_index_.find(key);
  if (index == nullptr || *index >= inline_zsets_.size() ||
      &inline_zsets_[*index].zset != &zset) {
    return;
  }

  const auto remove_index = *index;
  inline_zset_index_.erase(inline_zsets_[remove_index].key);
  if (remove_index + 1 == inline_zsets_.size()) {
    inline_zsets_.pop_back();
    return;
  }

  inline_zsets_[remove_index] = std::move(inline_zsets_.back());
  inline_zsets_.pop_back();
  auto moved = inline_zset_index_.find(inline_zsets_[remove_index].key);
  if (moved != nullptr) {
    *moved = remove_index;
  }
}

ZSet* Store::find_zset(std::string_view key) noexcept {
  if (auto* inline_zset = find_inline_zset(key); inline_zset != nullptr) {
    return inline_zset;
  }
  if (overflow_zsets_.empty()) {
    return nullptr;
  }
  return overflow_zsets_.find(key);
}

const ZSet* Store::find_zset(std::string_view key) const noexcept {
  if (const auto* inline_zset = find_inline_zset(key); inline_zset != nullptr) {
    return inline_zset;
  }
  if (overflow_zsets_.empty()) {
    return nullptr;
  }
  return overflow_zsets_.find(key);
}

ZSet& Store::get_or_create_zset(std::string_view key) {
  if (auto* inline_zset = find_inline_zset(key); inline_zset != nullptr) {
    return *inline_zset;
  }

  if (!overflow_zsets_.empty()) {
    if (auto* overflow = overflow_zsets_.find(key); overflow != nullptr) {
      return *overflow;
    }
  }

  if (!inline_zset_slots_full()) {
    return emplace_inline_zset(key);
  }

  auto [zset, inserted] = overflow_zsets_.try_emplace(key, zset_options());
  (void)inserted;
  return *zset;
}

void Store::erase_if_empty(std::string_view key, const ZSet& zset) {
  if (!zset.empty()) {
    return;
  }

  erase_inline_zset_if(key, zset);
  overflow_zsets_.erase(key);
}

const ZSet* Store::find_member_layer_template() const noexcept {
  for (const auto& slot : inline_zsets_) {
    if (!slot.zset.empty() && !slot.zset.is_small()) {
      return &slot.zset;
    }
  }
  const ZSet* template_zset = nullptr;
  overflow_zsets_.for_each([&template_zset](const auto& entry) {
    if (template_zset == nullptr && !entry.second.empty() &&
        !entry.second.is_small()) {
      template_zset = &entry.second;
    }
  });
  return template_zset;
}

long long Store::zadd(std::string_view key, double score, std::string_view member) {
  auto& zset = get_or_create_zset(key);
  // Small (listpack) zsets are standalone blobs -- they never adopt a shared
  // member layer, so skip the template scan entirely (it is O(number of zsets)).
  if (!zset.is_small() && zset.empty()) {
    if (const ZSet* tmpl = find_member_layer_template(); tmpl != nullptr && tmpl != &zset) {
      if (const auto tmpl_score = tmpl->score(member);
          tmpl_score.has_value() && *tmpl_score == score) {
        zset.adopt_shared_member_layer_from(*tmpl);
      }
    }
  }
  return zset.add(score, member);
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

  // Byte-arena reclaim and swiss-index tombstone cleanup are now independent
  // (the id-stable arena compaction no longer rebuilds the index), so run both.
  (void)zset->compact_after_removal_if_needed(static_cast<std::size_t>(removed));
  (void)zset->cleanup_member_index_after_removal_if_needed(
      static_cast<std::size_t>(removed));
  erase_if_empty(key, *zset);

  return removed;
}

// ---- Hash ----

Hash* Store::find_hash(std::string_view key) noexcept {
  if (inline_hash_.has_value() && std::string_view(inline_hash_key_) == key) {
    return &*inline_hash_;
  }
  if (overflow_hashes_.empty()) {
    return nullptr;
  }
  return overflow_hashes_.find(key);
}

const Hash* Store::find_hash(std::string_view key) const noexcept {
  if (inline_hash_.has_value() && std::string_view(inline_hash_key_) == key) {
    return &*inline_hash_;
  }
  if (overflow_hashes_.empty()) {
    return nullptr;
  }
  return overflow_hashes_.find(key);
}

Hash& Store::get_or_create_hash(std::string_view key) {
  if (inline_hash_.has_value() && std::string_view(inline_hash_key_) == key) {
    return *inline_hash_;
  }
  if (!overflow_hashes_.empty()) {
    auto* overflow = overflow_hashes_.find(key);
    if (overflow != nullptr) {
      return *overflow;
    }
  }
  const HashOptions hash_options{
      .member_index_growth = options_.member_index_growth,
      .chunk_bytes = options_.hash_chunk_bytes,
  };
  if (!inline_hash_.has_value() && overflow_hashes_.empty()) {
    inline_hash_key_.assign(key);
    inline_hash_.emplace(hash_options);
    return *inline_hash_;
  }
  auto [hash, inserted] =
      overflow_hashes_.try_emplace(key, hash_options);
  (void)inserted;
  return *hash;
}

void Store::erase_if_empty(std::string_view key, const Hash& hash) {
  if (!hash.empty()) {
    return;
  }
  if (inline_hash_.has_value() && std::string_view(inline_hash_key_) == key &&
      &*inline_hash_ == &hash) {
    inline_hash_.reset();
    inline_hash_key_.clear();
    return;
  }
  overflow_hashes_.erase(key);
}

void Store::place_loaded_hash(std::string key, Hash&& hash) {
  if (!inline_hash_.has_value() && overflow_hashes_.empty()) {
    inline_hash_key_ = std::move(key);
    inline_hash_.emplace(std::move(hash));
  } else {
    overflow_hashes_.try_emplace(std::move(key), std::move(hash));
  }
}

int Store::hset(std::string_view key, std::string_view field,
                std::string_view value) {
  return get_or_create_hash(key).set(field, value);
}

int Store::hsetnx(std::string_view key, std::string_view field,
                  std::string_view value) {
  return get_or_create_hash(key).set_nx(field, value);
}

std::optional<std::string_view> Store::hget(std::string_view key,
                                            std::string_view field) const {
  const auto* hash = find_hash(key);
  if (hash == nullptr) {
    return std::nullopt;
  }
  return hash->get(field);
}

bool Store::hexists(std::string_view key, std::string_view field) const {
  const auto* hash = find_hash(key);
  return hash != nullptr && hash->contains(field);
}

bool Store::hdel(std::string_view key, std::string_view field) {
  auto* hash = find_hash(key);
  if (hash == nullptr) {
    return false;
  }
  const bool removed = hash->erase(field);
  erase_if_empty(key, *hash);
  return removed;
}

std::size_t Store::hlen(std::string_view key) const {
  const auto* hash = find_hash(key);
  return hash == nullptr ? 0 : hash->size();
}

std::optional<std::size_t> Store::hstrlen(std::string_view key,
                                          std::string_view field) const {
  const auto* hash = find_hash(key);
  if (hash == nullptr) {
    return std::size_t{0};
  }
  const auto value = hash->get(field);
  return value ? value->size() : std::size_t{0};
}

std::optional<long long> Store::hincrby(std::string_view key,
                                        std::string_view field,
                                        long long delta) {
  auto& hash = get_or_create_hash(key);
  long long current = 0;
  if (const auto value = hash.get(field); value) {
    const auto parsed = parse_i64(*value);
    if (!parsed) {
      erase_if_empty(key, hash);
      return std::nullopt;  // existing value is not an integer
    }
    current = *parsed;
  }
  constexpr auto kMax = std::numeric_limits<long long>::max();
  constexpr auto kMin = std::numeric_limits<long long>::min();
  if ((delta > 0 && current > kMax - delta) ||
      (delta < 0 && current < kMin - delta)) {
    erase_if_empty(key, hash);
    return std::nullopt;  // would overflow
  }
  const long long next = current + delta;
  hash.set(field, std::to_string(next));
  return next;
}

std::optional<HashMemoryStats> Store::hash_memory_stats(
    std::string_view key) const {
  const auto* hash = find_hash(key);
  if (hash == nullptr) {
    return std::nullopt;
  }
  return hash->memory_stats();
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
  if (auto* zset = find_zset(key); zset != nullptr) {
    const auto before = zset->memory_stats().total_allocated_bytes;
    zset->compact(member_index_density);
    const auto after = zset->memory_stats().total_allocated_bytes;
    return before > after ? before - after : 0;
  }
  if (auto* hash = find_hash(key); hash != nullptr) {
    const auto before = hash->memory_stats().total_allocated_bytes;
    hash->compact(member_index_density);
    // Hand the freed arena/index pages back to the OS, matching ZSet::compact
    // (a no-op under jemalloc, effective under glibc -- goblin's default).
    release_unused_heap_pages();
    const auto after = hash->memory_stats().total_allocated_bytes;
    return before > after ? before - after : 0;
  }
  return std::nullopt;
}

namespace {

// A score is written at the zset's current width (bit-preserving via u16/u32 for
// the signed integer widths), and read back the same way -- smaller, faster
// snapshots for integer-scored zsets. The width tag precedes the members.
void write_zset_score(snapshot::Writer& writer, ScoreWidth width, double score) {
  switch (width) {
    case ScoreWidth::I16:
      writer.u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(score)));
      return;
    case ScoreWidth::I32:
      writer.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(score)));
      return;
    case ScoreWidth::F64:
      writer.f64(score);
      return;
  }
}

[[nodiscard]] double read_zset_score(snapshot::Reader& reader, ScoreWidth width) {
  switch (width) {
    case ScoreWidth::I16:
      return static_cast<double>(static_cast<std::int16_t>(reader.u16()));
    case ScoreWidth::I32:
      return static_cast<double>(static_cast<std::int32_t>(reader.u32()));
    case ScoreWidth::F64:
      return reader.f64();
  }
  return 0.0;
}

}  // namespace

void ZSet::save(snapshot::Writer& writer, bool with_accelerator) const {
  writer.u8(static_cast<std::uint8_t>(options_->rank_cache_mode));
  writer.u8(options_->score_string_cache ? 1 : 0);
  writer.f64(options_->member_index_growth);

  if (const auto* lp = small_ptr()) {
    // Listpack mode: the same canonical (width, per-member score+bytes) format,
    // read straight from the blob; a listpack carries no accelerator.
    const auto width = lp->score_width();
    writer.u8(static_cast<std::uint8_t>(width));
    writer.u64(static_cast<std::uint64_t>(lp->size()));
    lp->for_each([&writer, width](double score, std::string_view member) {
      write_zset_score(writer, width, score);
      writer.str(member);
    });
    writer.u8(0);  // no accelerator
    return;
  }

  const auto width = member_storage()->score_width();
  writer.u8(static_cast<std::uint8_t>(width));

  const auto member_count = static_cast<std::uint32_t>(member_storage()->size());
  writer.u64(member_count);

  // Canonical layer: (score@width, member bytes) in member-id order. This alone
  // is enough to reconstruct the zset (at whatever width its scores warrant).
  for (std::uint32_t id = 0; id < member_count; ++id) {
    write_zset_score(writer, width, member_storage()->score(id));
    writer.str(member_storage()->view(id));
  }

  // Accelerator layer (optional): the member index dumped verbatim, then member
  // ids in ascending score order (so the score index rebuilds without sorting).
  writer.u8(with_accelerator ? 1 : 0);
  if (with_accelerator) {
    members().write_accelerator(writer);
    const auto ordered = entries().range(0, entries().size());
    for (const auto& entry : ordered) {
      writer.u32(entry.member_id);
    }
  }
}

ZSet ZSet::load(snapshot::Reader& reader, bool use_accelerator,
                const ZSetOptions* options) {
  // Per-zset config (rank cache, score-string cache, growth) was persisted, but
  // the loading server's configuration applies -- as it already did for chunk
  // size -- so read past it and use the store's options.
  (void)reader.u8();   // rank_cache_mode
  (void)reader.u8();   // score_string_cache
  (void)reader.f64();  // member_index_growth

  const auto width = static_cast<ScoreWidth>(reader.u8());

  ZSet zset(options);
  zset.ensure_full();  // loaded zsets rebuild into the full arena-shaped structure
  const auto member_count = static_cast<std::uint32_t>(reader.u64());
  zset.member_storage()->reserve(member_count);

  // Canonical rebuild: push_back assigns member ids 0..N-1 in the same order
  // they were written, so accelerator member ids stay valid. Scores are read at
  // the persisted width; push_back re-derives the narrowest width that fits (so a
  // set saved wide but now all-integer loads back narrow). Dead bytes are dropped.
  for (std::uint32_t id = 0; id < member_count; ++id) {
    const double score = read_zset_score(reader, width);
    const auto member = reader.str();
    (void)zset.member_storage()->push_back(member, score);
  }

  const bool accelerator_present = reader.u8() != 0;
  if (accelerator_present && use_accelerator) {
    zset.members().read_accelerator(reader, zset.member_storage());
    if (zset.members().size() != member_count) {
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
          ZSetScoreEntry{.score = zset.member_storage()->score(id), .member_id = id});
    }
    zset.entries().assign_sorted(ordered);
  } else {
    // Slow path: rebuild both indexes from the canonical members. The score
    // index is built with insert() so that equal scores order by member bytes
    // exactly as the live structure does. Any present-but-unused accelerator
    // bytes are bounded by the instruction's operand length and simply ignored.
    zset.members().reserve_for_density(member_count, kDefaultMemberIndexDensity);
    for (std::uint32_t id = 0; id < member_count; ++id) {
      zset.members().insert_packed(zset.member_storage()->view(id),
                                  ZSetMemberMeta{.member_id = id});
      zset.entries().insert(
          ZSetScoreEntry{.score = zset.member_storage()->score(id), .member_id = id});
    }
  }

  zset.rebind_indexes();
  return zset;
}

void Store::save(std::ostream& out, bool with_accelerator) const {
  // File header, then one section per data family. Only ZSET exists today.
  std::string header;
  snapshot::Writer writer(header);
  writer.bytes(snapshot::kMagic, sizeof(snapshot::kMagic));
  writer.u32(snapshot::kFormatVersion);
  writer.u32(0);  // file flags (reserved)
  writer.u32(2);  // section count (ZSET, then HASH)
  // ZSET section header: family, accelerator version (0 = this snapshot carries
  // no accelerator), and the hash identity the accelerator's swiss dump was
  // built with (a loader with a different std::hash must rebuild from canonical
  // rather than trust the dump).
  writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Zset));
  writer.u32(with_accelerator ? snapshot::kZsetAcceleratorVersion : 0);
  writer.u64(ZSetMemberIndex::hash_identity());
  out.write(header.data(), static_cast<std::streamsize>(header.size()));

  // ZSET section body: a stream of OP_ZSET instructions, then OP_END.
  std::string operands;
  auto emit_zset = [&out, &operands, with_accelerator](std::string_view key,
                                                       const ZSet& zset) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    zset.save(operand_writer, with_accelerator);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::ZsetOpcode::Zset));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };

  for (const auto& slot : inline_zsets_) {
    emit_zset(slot.key, slot.zset);
  }
  overflow_zsets_.for_each([&emit_zset](const std::pair<std::string, ZSet>& entry) {
    emit_zset(entry.first, entry.second);
  });

  const char end = static_cast<char>(snapshot::kOpEnd);
  out.write(&end, 1);

  // HASH section: header, then a stream of OP_HASH instructions, then OP_END.
  // The field index and the member index share a hash identity (same std::hash
  // probe), so the zset's identity gates the hash accelerator too.
  std::string hash_header;
  snapshot::Writer hash_writer(hash_header);
  hash_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Hash));
  hash_writer.u32(with_accelerator ? snapshot::kHashAcceleratorVersion : 0);
  hash_writer.u64(ZSetMemberIndex::hash_identity());
  out.write(hash_header.data(), static_cast<std::streamsize>(hash_header.size()));

  auto emit_hash = [&out, &operands, with_accelerator](std::string_view key,
                                                       const Hash& hash) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    hash.save(operand_writer, with_accelerator);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::HashOpcode::Hash));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };

  if (inline_hash_.has_value()) {
    emit_hash(inline_hash_key_, *inline_hash_);
  }
  overflow_hashes_.for_each([&emit_hash](const std::pair<std::string, Hash>& entry) {
    emit_hash(entry.first, entry.second);
  });
  out.write(&end, 1);

  if (!out) {
    throw snapshot::snapshot_error("snapshot write failed");
  }
}

Store::SaveStart Store::start_background_save(std::string path,
                                             bool with_accelerator) {
  if (background_save_child_ > 0) {
    return SaveStart::AlreadyRunning;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return SaveStart::ForkFailed;
  }

  if (child == 0) {
    // Child: a copy-on-write snapshot of the store, frozen at fork time. Write
    // to a temp file, fsync, rename into place, and _exit without running the
    // parent's destructors or atexit handlers (they belong to the parent). The
    // inherited descriptors are left untouched -- blindly closing them is unsafe
    // on some platforms (macOS keeps runtime ports in low fds) -- and close on
    // _exit anyway.
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      _exit(1);
    }
    bool ok = true;
    {
      FdOutputStreambuf sink(fd);
      std::ostream out(&sink);
      try {
        save(out, with_accelerator);
        out.flush();
      } catch (...) {
        ok = false;
      }
      if (!out) {
        ok = false;
      }
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(temp.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
      ::unlink(temp.c_str());
    }
    _exit(ok ? 0 : 1);
  }

  background_save_child_ = child;
  background_save_path_ = std::move(path);
  return SaveStart::Started;
}

std::optional<Store::SaveOutcome> Store::reap_background_save() noexcept {
  if (background_save_child_ <= 0) {
    return std::nullopt;
  }
  int status = 0;
  const pid_t result = ::waitpid(background_save_child_, &status, WNOHANG);
  if (result == 0) {
    return std::nullopt;  // still running
  }
  const bool ok = result == background_save_child_ && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0;
  SaveOutcome outcome{.path = std::move(background_save_path_), .ok = ok};
  background_save_child_ = -1;
  background_save_path_.clear();
  return outcome;
}

SnapshotLoadStats Store::load(std::istream& in) {
  // Auto-detect the format by magic (requires a seekable stream).
  char magic[5] = {};
  in.read(magic, sizeof(magic));
  const auto got = in.gcount();
  in.clear();
  in.seekg(0);
  if (got == static_cast<std::streamsize>(sizeof(magic)) &&
      std::string_view(magic, sizeof(magic)) == "REDIS") {
    return rdb::import(*this, in);
  }
  return load_native(in);
}

void Store::clear() noexcept {
  inline_zsets_.clear();
  inline_zset_index_.clear();
  overflow_zsets_.clear();
  inline_hash_.reset();
  inline_hash_key_.clear();
  overflow_hashes_.clear();
}

SnapshotLoadStats Store::load_native(std::istream& in) {
  inline_zsets_.clear();
  inline_zset_index_.clear();
  overflow_zsets_.clear();
  inline_hash_.reset();
  inline_hash_key_.clear();
  overflow_hashes_.clear();

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
      const auto section_header = read_exact(4 + 4 + 8);
      snapshot::Reader section_reader(section_header);
      const auto section_type = section_reader.u32();
      const auto section_version = section_reader.u32();
      const auto hash_identity = section_reader.u64();

      const bool is_zset =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::Zset);
      const bool is_hash =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::Hash);
      const bool zset_use_accelerator =
          is_zset && section_version == snapshot::kZsetAcceleratorVersion &&
          hash_identity == ZSetMemberIndex::hash_identity();
      const bool hash_use_accelerator =
          is_hash && section_version == snapshot::kHashAcceleratorVersion &&
          hash_identity == ZSetMemberIndex::hash_identity();
      if (is_zset) {
        stats.used_accelerator = zset_use_accelerator;
      }

      // Interpret the section's instruction stream until OP_END.
      for (;;) {
        const auto opcode = static_cast<std::uint8_t>(read_exact(1)[0]);
        if (opcode == snapshot::kOpEnd) {
          break;
        }
        const auto frame = read_exact(8 + 4);  // operand length (u64) + CRC32C (u32)
        snapshot::Reader frame_reader(frame);
        const auto operand_size = frame_reader.u64();
        const auto expected_checksum = frame_reader.u32();
        const auto operands = read_exact(operand_size);
        if (snapshot::checksum(operands) != expected_checksum) {
          throw snapshot::snapshot_error("snapshot checksum mismatch");
        }

        if (is_zset &&
            opcode == static_cast<std::uint8_t>(snapshot::ZsetOpcode::Zset)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          ZSet zset =
              ZSet::load(reader, zset_use_accelerator, zset_options());
          stats.members += zset.size();
          place_loaded_zset(std::move(key), std::move(zset));
          ++stats.keys;
        } else if (is_hash && opcode == static_cast<std::uint8_t>(
                                            snapshot::HashOpcode::Hash)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          Hash hash =
              Hash::load(reader, hash_use_accelerator, options_.hash_chunk_bytes);
          stats.members += hash.size();
          place_loaded_hash(std::move(key), std::move(hash));
          ++stats.keys;
        }
        // else: unknown opcode or section -- already consumed, skip.
      }
    }
    return stats;
  } catch (...) {
    inline_zsets_.clear();
    inline_zset_index_.clear();
    overflow_zsets_.clear();
    inline_hash_.reset();
    inline_hash_key_.clear();
    overflow_hashes_.clear();
    throw;
  }
}

void Store::place_loaded_zset(std::string key, ZSet&& zset) {
  if (!inline_zset_slots_full()) {
    const auto index = inline_zsets_.size();
    inline_zsets_.push_back(
        InlineZsetSlot{.key = key, .zset = std::move(zset)});
    inline_zset_index_.try_emplace(inline_zsets_.back().key, index);
    return;
  }

  overflow_zsets_.try_emplace(std::move(key), std::move(zset));
}

StoreMemoryStats Store::memory_stats() const noexcept {
  StoreMemoryStats stats;
  stats.inline_zset_count = inline_zsets_.size();
  stats.overflow_zset_count = overflow_zsets_.size();
  stats.overflow_zset_capacity = overflow_zsets_.capacity();
  stats.overflow_table_allocated_bytes = overflow_zsets_.allocated_bytes();
  stats.inline_zset_index_allocated_bytes = inline_zset_index_.allocated_bytes();
  for (const auto& slot : inline_zsets_) {
    stats.inline_zset_allocated_bytes +=
        slot.zset.memory_stats().total_allocated_bytes;
  }
  overflow_zsets_.for_each([&stats](const auto& entry) {
    stats.overflow_zset_allocated_bytes +=
        entry.second.memory_stats().total_allocated_bytes;
  });
  stats.total_allocated_bytes = stats.overflow_table_allocated_bytes +
                                stats.inline_zset_index_allocated_bytes +
                                stats.inline_zset_allocated_bytes +
                                stats.overflow_zset_allocated_bytes;
  return stats;
}

std::string format_score(double score) {
  return score_format::format(score);
}

}  // namespace goblin::core
