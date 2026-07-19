#include "goblin/core/store.hpp"

#include "goblin/core/hugetlb.hpp"
#include "goblin/core/parse_int.hpp"
#include "goblin/core/rdb.hpp"

#include <algorithm>
#include <cassert>
#include <array>
#include <charconv>
#include <chrono>
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
using goblin::core::parse_i64;

// Strict finite double parse for INCRBYFLOAT (rejects trailing junk, inf, nan).
[[nodiscard]] std::optional<double> parse_double(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  double value = 0.0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

// Canonical text for an INCRBYFLOAT result: the shortest round-trippable form
// (3, 3.14, 5000) with no trailing decimal point or zeros.
[[nodiscard]] std::string format_incrbyfloat(double value) {
  char buffer[64];
  const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  (void)ec;
  return std::string(buffer, ptr);
}

// Stack buffer for integer field values (HINCRBY). 20 digits + sign + NUL.
[[nodiscard]] std::string_view format_i64(long long value,
                                          char (&buffer)[32]) noexcept {
  const auto [ptr, ec] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  (void)ec;
  return std::string_view(buffer, static_cast<std::size_t>(ptr - buffer));
}

[[nodiscard]] long long normalize_index(long long index, std::size_t size) noexcept {
  if (index < 0) {
    index += static_cast<long long>(size);
  }
  return index;
}

struct ListRange {
  std::size_t first{0};
  std::size_t count{0};
};

[[nodiscard]] std::optional<ListRange> normalize_list_range(
    long long start, long long stop, std::size_t size) noexcept {
  if (size == 0) {
    return std::nullopt;
  }
  const auto n = static_cast<long long>(size);
  if (start < 0) {
    start += n;
  }
  if (stop < 0) {
    stop += n;
  }
  start = std::max<long long>(0, start);
  if (stop < 0) {
    return std::nullopt;
  }
  stop = std::min(n - 1, stop);
  if (start >= n || stop < start) {
    return std::nullopt;
  }
  return ListRange{.first = static_cast<std::size_t>(start),
                   .count = static_cast<std::size_t>(stop - start + 1)};
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
void ZSet::init_empty(const ZSetOptions* options) {
  if (options->listpack_max_entries == 0) {
    auto member_layer = std::make_shared<ZSetMemberLayer>(
        options->score_string_cache, options->member_chunk_bytes,
        options->member_index_growth);
    auto score_index = std::make_shared<ZSetScoreIndex>(
        member_layer->storage.get(), options->rank_cache_mode,
        ZSetScoreIndex::kDefaultBlockHintNarrowLimit, options->score_index_load);
    rep_ = std::make_unique<FullState>(
        FullState{std::move(member_layer), std::move(score_index), options});
    rebind_indexes();
  } else {
    // Empty listpack; max_entries is a store-global passed to add(), not stored.
    rep_ = CompactListpack{};
  }
}

namespace {
// Shared default options for standalone, default-constructed zsets (the store
// points its own zsets at its owned options instead).
const ZSetOptions kDefaultZSetOptions{};
}  // namespace

const ZSetOptions* ZSet::default_options() noexcept {
  return &kDefaultZSetOptions;
}

ZSet::ZSet() { init_empty(&kDefaultZSetOptions); }

ZSet::ZSet(const ZSetOptions* options) { init_empty(options); }

// The full state lives behind a unique_ptr, so moving a zset just moves the
// pointer -- the FullState object (and every index pointer into it) stays put,
// so no rebind is needed, and the moved-from is left an empty (null) rep that
// is only ever destroyed.
ZSet::ZSet(ZSet&& other) noexcept : rep_(std::move(other.rep_)) {}

ZSet& ZSet::operator=(ZSet&& other) noexcept {
  if (this != &other) {
    rep_ = std::move(other.rep_);
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
    fs.member_layer =
        fs.member_layer->clone_shallow(fs.options->member_index_growth);
  }
  if (kind == WriteKind::Structural) {
    member_storage()->ensure_unique_arena();
  }
  if (fs.score_index.use_count() > 1) {
    auto cloned = std::make_shared<ZSetScoreIndex>(
        member_storage(), fs.options->rank_cache_mode,
        ZSetScoreIndex::kDefaultBlockHintNarrowLimit, fs.options->score_index_load);
    cloned->copy_blocks_from(entries());
    fs.score_index = std::move(cloned);
  }
  rebind_indexes();
}

void ZSet::adopt_shared_member_layer_from(const ZSet& source) {
  // Adopting a shared full member layer makes this a full zset.
  const ZSetOptions* options = source.full().options;
  auto member_layer = source.full().member_layer;
  auto copied = std::make_shared<ZSetScoreIndex>(
      member_layer->storage.get(), options->rank_cache_mode,
      ZSetScoreIndex::kDefaultBlockHintNarrowLimit, options->score_index_load);
  copied->copy_blocks_from(source.entries());
  rep_ = std::make_unique<FullState>(
      FullState{std::move(member_layer), std::move(copied), options});
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
void ZSet::ensure_full(const ZSetOptions* options) {
  auto* small = small_ptr();
  if (small == nullptr) {
    return;
  }
  CompactListpack lp = std::move(*small);
  auto member_layer = std::make_shared<ZSetMemberLayer>(
      options->score_string_cache, options->member_chunk_bytes,
      options->member_index_growth);
  auto score_index = std::make_shared<ZSetScoreIndex>(
      member_layer->storage.get(), options->rank_cache_mode,
      ZSetScoreIndex::kDefaultBlockHintNarrowLimit, options->score_index_load);
  rep_ = std::make_unique<FullState>(
      FullState{std::move(member_layer), std::move(score_index), options});
  rebind_indexes();
  lp.for_each([this](double score, std::string_view member) {
    const auto member_id = allocate_member_id(member, score);
    members().insert(member_view(member_id),
                     ZSetMemberMeta{.member_id = member_id});
    entries().insert(ZSetScoreEntry{.score = score, .member_id = member_id, .prefix = zset_member_prefix(member)});
  });
}

void ZSet::maybe_demote_to_small() {
  if (is_small()) {
    return;
  }
  const ZSetOptions* options = full().options;
  if (options->listpack_max_entries == 0 || options->score_string_cache) {
    return;
  }
  const auto member_count = static_cast<std::size_t>(member_storage()->size());
  if (member_count > options->listpack_max_entries) {
    return;
  }

  CompactListpack lp;
  const auto ordered = entries().range(0, entries().size());
  for (const auto& entry : ordered) {
    const auto member = member_storage()->view(entry.member_id);
    const auto result =
        lp.add(entry.score, member, options->listpack_max_entries);
    if (result.needs_full) {
      return;
    }
  }
  rep_ = std::move(lp);
}

// A ZADD whose score falls outside the zset's current score width auto-promotes
// it (i16->i32->f64) inside the member storage, which rebuilds the score array
// O(n). DANGER: when the zset is copy-on-write shared (see
// adopt_shared_member_layer_from), ensure_unique_mutable_state below already forks
// the SoA O(n) -- so a *promoting* ZADD on a large shared set pays the fork plus
// the width rebuild (2*O(n) and transient extra memory), a latency/memory spike a
// single fractional or out-of-range score can trigger. Widening is one-way;
// GOBLIN.OPTIMIZE reclaims it by scanning and demoting to the narrowest fit.
int ZSet::add(double score, std::string_view member,
              const ZSetOptions* options) {
  const auto result = add(score, member, ZAddOptions{}, options);
  return result.added ? 1 : 0;
}

ZAddMutation ZSet::add(double score, std::string_view member,
                       ZAddOptions add_options,
                       const ZSetOptions* options) {
  if (std::isnan(score)) {
    return {.status = ZAddStatus::InvalidScore};
  }

  ZSetMemberMeta* meta = nullptr;
  std::optional<double> old_score;
  if (const auto* lp = small_ptr()) {
    old_score = lp->score(member);
  } else {
    meta = members().find(member);
    if (meta != nullptr) {
      old_score = member_storage()->score(meta->member_id);
    }
  }
  const bool existed = old_score.has_value();
  double target_score = score;
  if (add_options.increment && existed) {
    target_score = *old_score + score;
    if (std::isnan(target_score)) {
      return {.status = ZAddStatus::InvalidScore};
    }
  }

  if ((existed && add_options.nx) || (!existed && add_options.xx) ||
      (existed && add_options.gt && target_score <= *old_score) ||
      (existed && add_options.lt && target_score >= *old_score)) {
    return {.status = ZAddStatus::Skipped};
  }

  if (existed && target_score == *old_score) {
    return {.status = ZAddStatus::Applied,
            .added = false,
            .changed = false,
            .score = target_score};
  }

  if (auto* lp = small_ptr()) {
    const auto result =
        lp->add(target_score, member, options->listpack_max_entries);
    if (!result.needs_full) {
      return {.status = ZAddStatus::Applied,
              .added = result.changed && !result.existed,
              .changed = result.changed,
              .score = target_score};
    }
    ensure_full(options);  // outgrew the listpack; fall to the full add path
  }

  if (meta == nullptr) {
    meta = members().find(member);
  }
  if (meta != nullptr) {
    const auto member_id = meta->member_id;
    const auto stored_score = member_storage()->score(member_id);

    ensure_unique_mutable_state(WriteKind::ScoreUpdate);

    member_storage()->set_score(member_id, target_score);
    const bool rescored = entries().rescore(
        ZSetScoreEntry{.score = stored_score, .member_id = member_id,
                       .prefix = zset_member_prefix(member)},
        ZSetScoreEntry{.score = target_score, .member_id = member_id,
                       .prefix = zset_member_prefix(member)});
    assert(rescored);
    if (!rescored) {
      return {.status = ZAddStatus::Skipped};
    }
    return {.status = ZAddStatus::Applied,
            .added = false,
            .changed = true,
            .score = target_score};
  }

  ensure_unique_mutable_state(WriteKind::Structural);

  const auto member_id = allocate_member_id(member, target_score);
  const auto view = member_view(member_id);
  members().insert(view, ZSetMemberMeta{.member_id = member_id});
  entries().insert(ZSetScoreEntry{.score = target_score,
                                  .member_id = member_id,
                                  .prefix = zset_member_prefix(member)});

  return {.status = ZAddStatus::Applied,
          .added = true,
          .changed = true,
          .score = target_score};
}

std::size_t ZSet::remove_by_score_range(double min, bool min_exclusive, double max,
                                        bool max_exclusive) {
  // Collect the matching member names first: removal below swap-moves member ids,
  // so ids must not be held across removes -- names are stable. The listpack form
  // is tiny (a cheap full scan); the full form seeks via the score index in
  // O(log n + matched).
  std::vector<std::string> names;
  const auto in_range = [&](double score) {
    return (min_exclusive ? score > min : score >= min) &&
           (max_exclusive ? score < max : score <= max);
  };
  if (small_ptr() != nullptr) {
    for_range_values(0, -1, [&](std::string_view member, double score) {
      if (in_range(score)) {
        names.emplace_back(member);
      }
    });
  } else {
    const auto* storage = member_storage();
    entries().for_score_range(
        min, min_exclusive, max, max_exclusive,
        [&](std::uint32_t id) { names.emplace_back(storage->view(id)); });
  }
  for (const auto& name : names) {
    (void)remove(name);
  }
  return names.size();
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
        entries().erase_one(ZSetScoreEntry{.score = old_score, .member_id = member_id, .prefix = zset_member_prefix(member)});
    assert(removed);
    if (!removed) {
      return false;
    }

    const bool erased = members().erase_at_index(*slot);
    assert(erased);
    member_storage()->orphan(member_id);
    member_storage()->pop_back();
    maybe_demote_to_small();
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
  maybe_demote_to_small();
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

std::optional<ZSetRangeBounds> ZSet::score_range_bounds(
    double min, bool min_exclusive, double max,
    bool max_exclusive) const {
  if (empty() || std::isnan(min) || std::isnan(max) || min > max ||
      (min == max && (min_exclusive || max_exclusive))) {
    return std::nullopt;
  }

  if (const auto* lp = small_ptr()) {
    std::size_t position = 0;
    std::size_t first = 0;
    std::size_t count = 0;
    bool found = false;
    lp->for_each([&](double score, std::string_view) {
      const bool above_min = min_exclusive ? score > min : score >= min;
      const bool below_max = max_exclusive ? score < max : score <= max;
      if (above_min && below_max) {
        if (!found) {
          first = position;
          found = true;
        }
        ++count;
      }
      ++position;
    });
    if (!found) {
      return std::nullopt;
    }
    return ZSetRangeBounds{.first = first, .count = count};
  }

  const auto [first, count] =
      entries().score_range_bounds(min, min_exclusive, max, max_exclusive);
  if (count == 0) {
    return std::nullopt;
  }
  return ZSetRangeBounds{.first = first, .count = count};
}

std::optional<ZSetRangeBounds> ZSet::score_range_slice(
    double min, bool min_exclusive, double max, bool max_exclusive,
    bool reverse, std::size_t offset,
    std::optional<std::size_t> limit) const {
  const auto ascending =
      score_range_bounds(min, min_exclusive, max, max_exclusive);
  if (!ascending || offset >= ascending->count ||
      (limit.has_value() && *limit == 0)) {
    return std::nullopt;
  }

  const auto available = ascending->count - offset;
  const auto count = limit.has_value() ? std::min(available, *limit) : available;
  const auto first = reverse
                         ? size() - (ascending->first + ascending->count) + offset
                         : ascending->first + offset;
  return ZSetRangeBounds{.first = first, .count = count};
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

ZSetMemoryStats ZSet::memory_stats(const ZSetOptions* options) const noexcept {
  ZSetMemoryStats stats;
  stats.member_count = size();
  if (const auto* lp = small_ptr()) {
    stats.rank_cache_mode = options->rank_cache_mode;
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
    stats.member_storage_dead_bytes = member_storage()->dead_bytes();
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
  const ZSetOptions* options = full().options;  // capture before rep_ is rebuilt
  const auto old_entries = entries().range(0, entries().size());

  std::size_t member_bytes = 0;
  for (const auto& old_entry : old_entries) {
    member_bytes += member_view(old_entry.member_id).size();
  }

  auto new_layer = std::make_shared<ZSetMemberLayer>(
      options->score_string_cache, options->member_chunk_bytes,
      options->member_index_growth);
  new_layer->storage->reserve(old_entries.size());
  new_layer->storage->reserve_bytes(member_bytes);
  if (options->score_string_cache) {
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

  ZSetScoreIndex new_score_index(new_layer->storage.get(), options->rank_cache_mode,
                                 ZSetScoreIndex::kDefaultBlockHintNarrowLimit,
                                 options->score_index_load);
  new_score_index.assign_sorted(new_entries);

  rep_ = std::make_unique<FullState>(FullState{
      std::move(new_layer),
      std::make_shared<ZSetScoreIndex>(std::move(new_score_index)), options});
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

namespace {

[[nodiscard]] ZSetOptions build_zset_options(const StoreOptions& options) {
  return ZSetOptions{
      .rank_cache_mode = options.rank_cache_mode,
      .score_string_cache = options.score_string_cache,
      .member_index_growth = options.member_index_growth,
      .member_chunk_bytes = options.zset_chunk_bytes,
      .score_index_load = options.zset_score_index_load,
      // The score-string cache preserves exact input text, which the numeric
      // listpack can't -- so enabling it keeps zsets in the full structure.
      .listpack_max_entries = options.score_string_cache
                                  ? std::size_t{0}
                                  : options.zset_listpack_max_entries,
  };
}

[[nodiscard]] HashOptions build_hash_options(const StoreOptions& options) {
  return HashOptions{
      .member_index_growth = options.member_index_growth,
      .chunk_bytes = options.hash_chunk_bytes,
      .compaction_knapsack = options.hash_compaction_knapsack,
      .string_encoding = options.string_encoding,
      .compaction_work_budget = options.hash_compaction_work_budget,
      .listpack_max_entries = options.hash_listpack_max_entries,
      .realtime_index_bytes = options.realtime_hash_index_bytes,
  };
}

[[nodiscard]] ListOptions build_list_options(const StoreOptions& options) {
  return ListOptions{
      .implementation = options.list_implementation,
      .chunk_bytes = options.list_chunk_bytes,
      .listpack_max_entries = options.list_listpack_max_entries,
      .max_density = options.list_max_density,
      .resize_growth = options.list_resize_growth,
      .string_encoding = options.string_encoding,
      .string_compression = StringCompressionMode::AllowLz4,
  };
}

[[nodiscard]] SetOptions build_set_options(const StoreOptions& options) {
  return SetOptions{
      .member_index_growth = options.member_index_growth,
      .chunk_bytes = options.set_chunk_bytes,
      .string_encoding = options.string_encoding,
      .listpack_max_entries = options.set_listpack_max_entries,
  };
}

[[nodiscard]] ArrayOptions build_array_options(const StoreOptions& options) {
  return ArrayOptions{
      .implementation = options.array_implementation,
      .slice_slots = options.array_slice_slots,
      .dir_fanout = 0,  // same as slice_slots
      .initial_depth = options.array_initial_depth,
      .chunk_bytes = options.array_chunk_bytes,
      .arena_growth = options.member_index_growth,
      .realtime_arena_growth = options.realtime_array_growth,
      .string_encoding = options.string_encoding,
  };
}

}  // namespace

Store::Store(StoreOptions options)
    : options_(options),
      zset_options_(build_zset_options(options)),
      keyspace_(build_hash_options(options), build_list_options(options),
                build_set_options(options), build_array_options(options),
                options.string_encoding,
                options.real_time ? MemberIndexImplementation::Realtime
                                  : MemberIndexImplementation::Efficient,
                options.real_time ||
                    options.hash_implementation ==
                        HashImplementation::Realtime) {}

void Store::erase_if_empty(std::string_view key, const ZSet& zset) {
  if (!zset.empty()) {
    return;
  }
  (void)erase_key(key);
}

const ZSet* Store::find_member_layer_template() const noexcept {
  const ZSet* template_zset = nullptr;
  keyspace_.for_each_zset([&template_zset](std::string_view, const ZSet& zset) {
    if (template_zset == nullptr && !zset.empty() && !zset.is_small()) {
      template_zset = &zset;
    }
  });
  return template_zset;
}

long long Store::zadd(std::string_view key, double score, std::string_view member) {
  const ZAddItem item{.score = score, .member = member};
  return zadd(key, std::span<const ZAddItem>(&item, 1)).added;
}

ZAddResult Store::zadd(std::string_view key, std::span<const ZAddItem> items,
                       ZAddOptions options) {
  ZAddResult result;
  if (items.empty()) {
    return result;
  }
  // Keep the storage API atomic even when called below the RESP/SBE parsers.
  // Validate the complete batch before creating a key or applying its first pair.
  if (std::any_of(items.begin(), items.end(), [](const ZAddItem& item) {
        return std::isnan(item.score);
      })) {
    result.invalid_score = true;
    return result;
  }

  auto* existing = find_zset(key);
  if (existing == nullptr && options.xx) {
    return result;
  }

  auto& zset = existing != nullptr ? *existing : get_or_create_zset(key);
  // Small (listpack) zsets are standalone blobs -- they never adopt a shared
  // member layer, so skip the template scan entirely (it is O(number of zsets)).
  if (!options.nx && !options.xx && !options.gt && !options.lt &&
      !options.increment && !zset.is_small() && zset.empty()) {
    if (const ZSet* tmpl = find_member_layer_template(); tmpl != nullptr && tmpl != &zset) {
      if (const auto tmpl_score = tmpl->score(items.front().member);
          tmpl_score.has_value() && *tmpl_score == items.front().score) {
        zset.adopt_shared_member_layer_from(*tmpl);
      }
    }
  }

  for (const auto& item : items) {
    const auto mutation =
        zset.add(item.score, item.member, options, zset_options());
    if (mutation.status == ZAddStatus::InvalidScore) {
      result.invalid_score = true;
      return result;
    }
    if (mutation.status == ZAddStatus::Skipped) {
      continue;
    }
    result.added += mutation.added ? 1 : 0;
    result.changed += mutation.changed ? 1 : 0;
    if (options.increment) {
      result.increment_score = mutation.score;
    }
  }
  if (result.changed != 0) {
    signal_key_modified(key);
  }
  return result;
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
  if (removed != 0 && !zset->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *zset);

  return removed;
}

std::vector<ZSetOwnedEntry> Store::zpop(std::string_view key,
                                       std::size_t count, bool maximum) {
  std::vector<ZSetOwnedEntry> popped;
  auto* zset = find_zset(key);
  if (zset == nullptr || count == 0) {
    return popped;
  }

  const auto take = std::min(count, zset->size());
  popped.reserve(take);
  const ZSetRangeBounds bounds{.first = 0, .count = take};
  zset->for_position_range(
      bounds, maximum,
      [&popped](std::string_view member, double score, std::string_view) {
        popped.push_back(ZSetOwnedEntry{.member = std::string(member),
                                        .score = score});
      });
  for (const auto& entry : popped) {
    const bool removed = zset->remove(entry.member);
    assert(removed);
    (void)removed;
  }
  (void)zset->compact_after_removal_if_needed(popped.size());
  (void)zset->cleanup_member_index_after_removal_if_needed(popped.size());
  if (!popped.empty() && !zset->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *zset);
  return popped;
}

long long Store::zremrangebyscore(std::string_view key, double min,
                                  bool min_exclusive, double max,
                                  bool max_exclusive) {
  auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }
  const auto removed =
      zset->remove_by_score_range(min, min_exclusive, max, max_exclusive);
  (void)zset->compact_after_removal_if_needed(removed);
  (void)zset->cleanup_member_index_after_removal_if_needed(removed);
  if (removed != 0 && !zset->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *zset);
  return static_cast<long long>(removed);
}

bool Store::zwindow(std::string_view key, double now, double cutoff,
                    long long limit, std::string_view member,
                    std::uint64_t when_ms, std::uint64_t now_ms) {
  // Evict entries older than the window (score <= cutoff), then admit iff there is
  // room; the trailing TTL re-arm reaps an idle key. One keyspace lookup covers the
  // whole op: the former version re-found the key for zcard and zadd and could
  // erase then immediately recreate a fully-cycled window. Here the zset is held
  // across evict -> size check -> add.
  if (ZSet* zset = find_zset(key); zset != nullptr) {
    const auto removed = zset->remove_by_score_range(
        -std::numeric_limits<double>::infinity(), false, cutoff, false);
    (void)zset->compact_after_removal_if_needed(removed);
    (void)zset->cleanup_member_index_after_removal_if_needed(removed);
    if (static_cast<long long>(zset->size()) >= limit) {
      if (removed != 0 && !zset->empty()) {
        signal_key_modified(key);
      }
      erase_if_empty(key, *zset);  // only reachable empty when limit <= 0
      return false;                // window already full -> reject
    }
    // Room to admit. Add on the same pointer (repopulating an emptied window, so it
    // is never erased). Intentionally not Store::zadd: its shared-member-layer
    // adoption is useless for per-request members and would cost an O(#zsets) scan.
    (void)zset->add(now, member, zset_options());
    (void)expire_at_ms(key, when_ms, now_ms);
    return true;
  }
  // No such key -> a fresh window; a non-positive limit admits nothing.
  if (limit <= 0) {
    return false;
  }
  (void)zadd(key, now, member);
  (void)expire_at_ms(key, when_ms, now_ms);
  return true;
}

// ---- Hash ----

void Store::erase_if_empty(std::string_view key, const Hash& hash) {
  if (!hash.empty()) {
    return;
  }
  (void)erase_key(key);
}

// ---- Set ----

void Store::erase_if_empty(std::string_view key, const Set& set) {
  if (!set.empty()) {
    return;
  }
  (void)erase_key(key);
}

void Store::place_loaded_set(std::string key, Set&& set) {
  (void)keyspace_.place_loaded_set(key, std::move(set));
}

void Store::place_loaded_array(std::string key, Array&& array) {
  (void)keyspace_.place_loaded_array(key, std::move(array));
}

long long Store::sadd(std::string_view key,
                      std::span<const std::string_view> members) {
  if (members.empty()) {
    return 0;
  }
  const auto added = get_or_create_set(key).add_many(members);
  if (added != 0) {
    signal_key_modified(key);
  }
  return added;
}

long long Store::srem(std::string_view key,
                      std::span<const std::string_view> members) {
  auto* set = find_set(key);
  if (set == nullptr) {
    return 0;
  }
  const auto removed = static_cast<long long>(set->erase_many(members));
  if (removed != 0 && !set->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *set);
  return removed;
}

long long Store::scard(std::string_view key) const {
  const auto* set = find_set(key);
  return set == nullptr ? 0 : static_cast<long long>(set->size());
}

bool Store::sismember(std::string_view key, std::string_view member) const {
  const auto* set = find_set(key);
  return set != nullptr && set->contains(member);
}

std::optional<SetMemoryStats> Store::set_memory_stats(
    std::string_view key) const {
  const auto* set = find_set(key);
  if (set == nullptr) {
    return std::nullopt;
  }
  return set->memory_stats();
}

std::optional<std::string> Store::spop(std::string_view key) {
  auto* set = find_set(key);
  if (set == nullptr) {
    return std::nullopt;
  }
  auto member = set->pop();
  if (member && !set->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *set);
  return member;
}

std::vector<std::string> Store::spop(std::string_view key, std::size_t count) {
  auto* set = find_set(key);
  if (set == nullptr) {
    return {};
  }
  auto members = set->pop_many(count);
  if (!members.empty() && !set->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *set);
  return members;
}

std::optional<std::string> Store::srandmember(std::string_view key) const {
  const auto* set = find_set(key);
  if (set == nullptr) {
    return std::nullopt;
  }
  return set->random_member();
}

std::vector<std::string> Store::srandmember(std::string_view key,
                                           std::size_t count,
                                           bool unique) const {
  const auto* set = find_set(key);
  if (set == nullptr) {
    return {};
  }
  return set->random_members(count, unique);
}

int Store::smove(std::string_view source, std::string_view destination,
                 std::string_view member) {
  // Same-key SMOVE is an existence check (Redis semantics).
  if (source == destination) {
    const auto* set = find_set(source);
    return set != nullptr && set->contains(member) ? 1 : 0;
  }
  auto* src = find_set(source);
  if (src == nullptr || !src->contains(member)) {
    return 0;
  }
  // Materialize before erase: the member view is invalidated by swap-remove.
  const auto owned = std::string(member);
  if (!src->erase(owned)) {
    return 0;
  }
  if (!src->empty()) {
    signal_key_modified(source);
  }
  erase_if_empty(source, *src);
  (void)get_or_create_set(destination).add(owned);
  signal_key_modified(destination);
  return 1;
}

std::vector<std::string> Store::sinter(
    std::span<const std::string_view> keys) const {
  if (keys.empty()) {
    return {};
  }
  // Pick the smallest existing set as the driver; a missing key empties the
  // intersection immediately.
  const Set* driver = nullptr;
  std::size_t driver_size = std::numeric_limits<std::size_t>::max();
  std::vector<const Set*> others;
  others.reserve(keys.size());
  for (const auto key : keys) {
    const auto* set = find_set(key);
    if (set == nullptr) {
      return {};
    }
    if (set->size() < driver_size) {
      if (driver != nullptr) {
        others.push_back(driver);
      }
      driver = set;
      driver_size = set->size();
    } else {
      others.push_back(set);
    }
  }
  assert(driver != nullptr);
  std::vector<std::string> out;
  out.reserve(driver_size);
  driver->for_each([&](EncodedStringView member) {
    const auto logical = member.to_string();
    for (const auto* other : others) {
      if (!other->contains(logical)) {
        return;
      }
    }
    out.push_back(std::move(logical));
  });
  return out;
}

std::vector<std::string> Store::sunion(
    std::span<const std::string_view> keys) const {
  // Dedup through a temporary Set so encoding/options match the store.
  Set merged(build_set_options(options_));
  for (const auto key : keys) {
    const auto* set = find_set(key);
    if (set == nullptr) {
      continue;
    }
    set->for_each([&](EncodedStringView member) {
      (void)merged.add(member.to_string());
    });
  }
  std::vector<std::string> out;
  out.reserve(merged.size());
  merged.for_each(
      [&](EncodedStringView member) { out.push_back(member.to_string()); });
  return out;
}

std::vector<std::string> Store::sdiff(
    std::span<const std::string_view> keys) const {
  if (keys.empty()) {
    return {};
  }
  const auto* first = find_set(keys[0]);
  if (first == nullptr) {
    return {};
  }
  std::vector<const Set*> subtract;
  subtract.reserve(keys.size() - 1);
  for (std::size_t i = 1; i < keys.size(); ++i) {
    if (const auto* set = find_set(keys[i])) {
      subtract.push_back(set);
    }
  }
  std::vector<std::string> out;
  out.reserve(first->size());
  first->for_each([&](EncodedStringView member) {
    const auto logical = member.to_string();
    for (const auto* other : subtract) {
      if (other->contains(logical)) {
        return;
      }
    }
    out.push_back(std::move(logical));
  });
  return out;
}

namespace {

[[nodiscard]] long long store_set_result(Store& store,
                                         std::string_view destination,
                                         std::vector<std::string> members) {
  // Overwrite destination regardless of prior type. Empty result deletes it.
  (void)store.del(destination);
  if (members.empty()) {
    return 0;
  }
  std::vector<std::string_view> views;
  views.reserve(members.size());
  for (const auto& member : members) {
    views.push_back(member);
  }
  return store.sadd(destination, views);
}

}  // namespace

long long Store::sinterstore(std::string_view destination,
                             std::span<const std::string_view> keys) {
  return store_set_result(*this, destination, sinter(keys));
}

long long Store::sunionstore(std::string_view destination,
                             std::span<const std::string_view> keys) {
  return store_set_result(*this, destination, sunion(keys));
}

long long Store::sdiffstore(std::string_view destination,
                            std::span<const std::string_view> keys) {
  return store_set_result(*this, destination, sdiff(keys));
}

long long Store::sintercard(std::span<const std::string_view> keys,
                            std::size_t limit) const {
  if (keys.empty()) {
    return 0;
  }
  const Set* driver = nullptr;
  std::size_t driver_size = std::numeric_limits<std::size_t>::max();
  std::vector<const Set*> others;
  others.reserve(keys.size());
  for (const auto key : keys) {
    const auto* set = find_set(key);
    if (set == nullptr) {
      return 0;
    }
    if (set->size() < driver_size) {
      if (driver != nullptr) {
        others.push_back(driver);
      }
      driver = set;
      driver_size = set->size();
    } else {
      others.push_back(set);
    }
  }
  assert(driver != nullptr);
  long long count = 0;
  const auto n = static_cast<std::uint32_t>(driver->size());
  for (std::uint32_t id = 0; id < n; ++id) {
    if (limit != 0 && static_cast<std::size_t>(count) >= limit) {
      break;
    }
    const auto logical = driver->at(id).to_string();
    bool in_all = true;
    for (const auto* other : others) {
      if (!other->contains(logical)) {
        in_all = false;
        break;
      }
    }
    if (in_all) {
      ++count;
    }
  }
  return count;
}


void Store::place_loaded_hash(std::string key, Hash&& hash) {
  (void)keyspace_.place_loaded_hash(key, std::move(hash));
}

int Store::hset(std::string_view key, std::string_view field,
                std::string_view value,
                std::optional<HashImplementation> implementation) {
  const int added = get_or_create_hash(key, implementation).set(field, value);
  signal_key_modified(key);
  return added;
}

long long Store::hset_many(
    std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::optional<HashImplementation> implementation) {
  if (fields.empty()) {
    return 0;
  }
  const auto added = get_or_create_hash(key, implementation).set_many(fields);
  signal_key_modified(key);
  return added;
}

int Store::hsetnx(std::string_view key, std::string_view field,
                  std::string_view value,
                  std::optional<HashImplementation> implementation) {
  const int added = get_or_create_hash(key, implementation).set_nx(field, value);
  if (added != 0) {
    signal_key_modified(key);
  }
  return added;
}

std::optional<EncodedStringView> Store::hget(std::string_view key,
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
  if (removed && !hash->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *hash);
  return removed;
}

std::size_t Store::hdel_many(
    std::string_view key, std::span<const std::string_view> fields) {
  auto* hash = find_hash(key);
  if (hash == nullptr) {
    return 0;
  }
  const auto removed = hash->erase_many(fields);
  if (removed != 0 && !hash->empty()) {
    signal_key_modified(key);
  }
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
                                        long long delta,
                                        std::optional<HashImplementation>
                                            implementation) {
  auto& hash = get_or_create_hash(key, implementation);
  long long current = 0;
  if (const auto value = hash.get(field); value) {
    // Integer field values are short; SSO covers the common decode path.
    const auto decoded = value->to_string();
    const auto parsed = parse_i64(decoded);
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
  char encode_buf[32];
  hash.set(field, format_i64(next, encode_buf));
  signal_key_modified(key);
  return next;
}

bool Store::hash_compare_and_delete(std::string_view key, std::string_view field,
                                    std::string_view expected) {
  auto* hash = find_hash(key);
  if (hash == nullptr) {
    return false;  // no such hash -> nothing to delete (0)
  }
  const auto current = hash->get(field);
  if (!current || *current != expected) {
    return false;  // field absent or value mismatch -> nothing deleted (0)
  }
  const bool removed = hash->erase(field);
  if (removed && !hash->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *hash);  // drop the key if that was its last field
  return removed;
}

std::optional<bool> Store::hash_set_if_greater(std::string_view key,
                                               std::string_view field, double value,
                                               std::string_view value_text) {
  // Read the current value without creating the hash: an absent key or field
  // counts as -inf, so a first write of any finite value wins.
  Hash* hash = find_hash(key);
  double current = -std::numeric_limits<double>::infinity();
  if (hash != nullptr) {
    if (const auto existing = hash->get(field)) {
      const auto decoded = existing->to_string();
      const auto parsed = parse_double(decoded);
      if (!parsed) {
        return std::nullopt;  // existing value is not a finite number
      }
      current = *parsed;
    }
  }
  if (!(value > current)) {
    return false;  // not strictly greater -> leave the field unchanged
  }
  // Store the raw text. Reuse the pointer we already found on the update path (the
  // common case), falling back to get_or_create only when the key is brand new --
  // this avoids a second keyspace lookup that hset() would repeat.
  if (hash != nullptr) {
    (void)hash->set(field, value_text);
  } else {
    (void)get_or_create_hash(key).set(field, value_text);
  }
  signal_key_modified(key);
  return true;
}

std::optional<HashMemoryStats> Store::hash_memory_stats(
    std::string_view key) const {
  const auto* hash = find_hash(key);
  if (hash == nullptr) {
    return std::nullopt;
  }
  return hash->memory_stats();
}

// ---- List ----

void Store::erase_if_empty(std::string_view key, const List& list) {
  if (!list.empty()) {
    return;
  }
  (void)erase_key(key);
}

void Store::place_loaded_list(std::string key, List&& list) {
  (void)keyspace_.place_loaded_list(key, std::move(list));
}

long long Store::lpush(std::string_view key,
                       std::span<const std::string_view> values,
                       bool only_if_exists,
                       std::optional<ListImplementation> implementation) {
  List* list = find_list(key);
  if (list == nullptr) {
    if (only_if_exists) {
      return 0;
    }
    list = &get_or_create_list(
        key, implementation.value_or(options_.list_implementation));
  }
  const auto size = static_cast<long long>(list->push_front(values));
  signal_key_modified(key);
  return size;
}

long long Store::rpush(std::string_view key,
                       std::span<const std::string_view> values,
                       bool only_if_exists,
                       std::optional<ListImplementation> implementation) {
  List* list = find_list(key);
  if (list == nullptr) {
    if (only_if_exists) {
      return 0;
    }
    list = &get_or_create_list(
        key, implementation.value_or(options_.list_implementation));
  }
  const auto size = static_cast<long long>(list->push_back(values));
  signal_key_modified(key);
  return size;
}

std::optional<std::string> Store::lpop(std::string_view key) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return std::nullopt;
  }
  auto value = list->pop_front();
  if (value && !list->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *list);
  return value;
}

std::optional<std::string> Store::rpop(std::string_view key) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return std::nullopt;
  }
  auto value = list->pop_back();
  if (value && !list->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *list);
  return value;
}

std::vector<std::string> Store::lpop(std::string_view key, std::size_t count) {
  std::vector<std::string> values;
  auto* list = find_list(key);
  if (list == nullptr || count == 0) {
    return values;
  }
  values = list->pop_front(count);
  if (!values.empty() && !list->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *list);
  return values;
}

std::vector<std::string> Store::rpop(std::string_view key, std::size_t count) {
  std::vector<std::string> values;
  auto* list = find_list(key);
  if (list == nullptr || count == 0) {
    return values;
  }
  values = list->pop_back(count);
  if (!values.empty() && !list->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *list);
  return values;
}

std::optional<std::string> Store::lmove(
    std::string_view source, std::string_view destination, bool source_front,
    bool destination_front,
    std::optional<ListImplementation> implementation) {
  auto* source_list = find_list(source);
  if (source_list == nullptr || source_list->empty()) {
    return std::nullopt;
  }

  const auto pop_source = [&]() {
    return source_front ? source_list->pop_front() : source_list->pop_back();
  };
  const auto restore_source = [&](std::string_view value) {
    if (source_front) {
      (void)source_list->push_front(value);
    } else {
      (void)source_list->push_back(value);
    }
  };

  auto value = pop_source();
  if (!value) {
    return std::nullopt;
  }

  if (source == destination) {
    try {
      if (destination_front) {
        (void)source_list->push_front(*value);
      } else {
        (void)source_list->push_back(*value);
      }
    } catch (...) {
      restore_source(*value);
      throw;
    }
    signal_key_modified(source);
    return value;
  }

  try {
    auto& destination_list = get_or_create_list(
        destination, implementation.value_or(options_.list_implementation));
    if (destination_front) {
      (void)destination_list.push_front(*value);
    } else {
      (void)destination_list.push_back(*value);
    }
  } catch (...) {
    restore_source(*value);
    throw;
  }

  if (source_list->empty()) {
    (void)erase_key(source);
  } else {
    signal_key_modified(source);
  }
  signal_key_modified(destination);
  return value;
}

std::size_t Store::llen(std::string_view key) const noexcept {
  const auto* list = find_list(key);
  return list == nullptr ? 0 : list->size();
}

std::optional<EncodedStringView> Store::lindex(std::string_view key,
                                               long long index) const noexcept {
  const auto* list = find_list(key);
  if (list == nullptr) {
    return std::nullopt;
  }
  index = normalize_index(index, list->size());
  if (index < 0 || static_cast<std::size_t>(index) >= list->size()) {
    return std::nullopt;
  }
  return list->at(static_cast<std::size_t>(index));
}

std::vector<EncodedStringView> Store::lrange(std::string_view key,
                                             long long start,
                                             long long stop) const {
  std::vector<EncodedStringView> values;
  const auto* list = find_list(key);
  if (list == nullptr) {
    return values;
  }
  const auto range = normalize_list_range(start, stop, list->size());
  if (!range) {
    return values;
  }
  values.reserve(range->count);
  list->for_range(range->first, range->count,
                  [&values](EncodedStringView value) {
                    values.push_back(value);
                  });
  return values;
}

Store::ListSetResult Store::lset(std::string_view key, long long index,
                                 std::string_view value) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return ListSetResult::MissingKey;
  }
  index = normalize_index(index, list->size());
  if (index < 0 || static_cast<std::size_t>(index) >= list->size()) {
    return ListSetResult::OutOfRange;
  }
  list->set(static_cast<std::size_t>(index), value);
  signal_key_modified(key);
  return ListSetResult::Stored;
}

void Store::ltrim(std::string_view key, long long start, long long stop) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return;
  }
  const auto range = normalize_list_range(start, stop, list->size());
  if (!range) {
    (void)erase_key(key);
    return;
  }
  list->trim(range->first, range->count);
  signal_key_modified(key);
}

std::size_t Store::lrem(std::string_view key, long long count,
                        std::string_view value) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return 0;
  }
  const auto removed = list->remove(value, count);
  if (removed != 0 && !list->empty()) {
    signal_key_modified(key);
  }
  erase_if_empty(key, *list);
  return removed;
}

long long Store::linsert(std::string_view key, bool before,
                         std::string_view pivot, std::string_view value) {
  auto* list = find_list(key);
  if (list == nullptr) {
    return 0;
  }
  const auto index = list->find_first(pivot);
  if (index) {
    list->insert(before ? *index : *index + 1, value);
    signal_key_modified(key);
    return static_cast<long long>(list->size());
  }
  return -1;
}

std::optional<ListMemoryStats> Store::list_memory_stats(
    std::string_view key) const {
  const auto* list = find_list(key);
  if (list == nullptr) {
    return std::nullopt;
  }
  return list->memory_stats();
}

// ---- String ----

void Store::set(std::string_view key, std::string_view value) {
  // One keyspace probe; clear TTL by the returned id (no second hash).
  const auto id = keyspace_.set_string(key, value);
  // SET clears any existing TTL (Redis default; SET ... KEEPTTL keeps it via
  // set_keep_ttl). Skip when no TTLs exist.
  if (!ttl_.empty()) {
    ttl_.clear(id);
  }
  signal_key_modified(key);
}

void Store::set_keep_ttl(std::string_view key, std::string_view value) {
  keyspace_.set_string(key, value);
  signal_key_modified(key);
}

bool Store::set_nx(std::string_view key, std::string_view value) {
  const bool stored = keyspace_.set_string_if_absent(key, value);
  if (stored) {
    signal_key_modified(key);
  }
  return stored;
}

std::optional<StringValueView> Store::get(std::string_view key) const noexcept {
  return keyspace_.get_string(key);
}

namespace {
// Materialize a stored encoded value into its Redis-visible string.
[[nodiscard]] std::string join_value(StringValueView value) {
  return value.to_string();
}

// Raw values compare in place; specialized encodings decode canonically.
[[nodiscard]] bool view_equals(StringValueView value, std::string_view s) {
  return value.equals(s);
}
}  // namespace

std::optional<std::string> Store::get_set(std::string_view key,
                                          std::string_view value) {
  std::optional<std::string> previous;
  if (const auto current = keyspace_.get_string(key)) {
    previous = join_value(*current);
  }
  keyspace_.set_string(key, value);
  signal_key_modified(key);
  return previous;
}

std::optional<std::string> Store::get_del(std::string_view key) {
  std::optional<std::string> previous;
  if (const auto current = keyspace_.get_string(key)) {
    previous = join_value(*current);
  }
  (void)erase_key(key);
  return previous;
}

bool Store::compare_and_delete(std::string_view key, std::string_view expected) {
  const auto current = keyspace_.get_string(key);
  if (!current || !view_equals(*current, expected)) {
    return false;  // missing key or mismatch -> nothing to delete (0)
  }
  return erase_key(key);
}

bool Store::compare_and_expire(std::string_view key, std::string_view expected,
                               std::uint64_t when_ms, std::uint64_t now) {
  const auto current = keyspace_.get_string(key);
  if (!current || !view_equals(*current, expected)) {
    return false;  // missing key or mismatch -> nothing to renew (0)
  }
  // The token matches, so the key exists: expire_at_ms sets its TTL (or deletes
  // it when when_ms is already past) and returns true.
  return expire_at_ms(key, when_ms, now);
}

bool Store::compare_and_set(std::string_view key, std::string_view expected,
                            std::string_view new_value) {
  const auto current = keyspace_.get_string(key);
  if (!current || !view_equals(*current, expected)) {
    return false;  // missing key or mismatch -> nothing to swap (0)
  }
  // Overwrite with the new value but keep any existing TTL -- a bare set() would
  // clear the expiry; set_keep_ttl is the KEEPTTL semantic.
  set_keep_ttl(key, new_value);
  return true;
}

Store::ClaimOutcome Store::claim(std::string_view claim_key,
                                 std::string_view result_key, std::string_view token,
                                 std::uint64_t when_ms, std::uint64_t now) {
  ClaimOutcome outcome;
  // An expired claim must free the slot before the NX check, and an expired result
  // must read as absent -- purge both at `now` (a cheap no-op when no TTLs exist).
  (void)purge_if_expired(claim_key, now);
  // SET claim_key = token NX: one keyspace lookup that both tests existence (of any
  // type) and stores on a win -- where a separate exists() + set() did two.
  if (set_nx(claim_key, token)) {
    (void)expire_at_ms(claim_key, when_ms, now);  // the EX arm of SET NX EX
    outcome.claimed = true;
    return outcome;
  }
  // The slot is already held -> return the stored result (GET result_key). NX never
  // WRONGTYPEs, but this fallthrough GET does when result_key is not a string.
  (void)purge_if_expired(result_key, now);
  if (const auto type = key_type(result_key);
      type.has_value() && *type != KeyType::String) {
    outcome.result_wrongtype = true;
    return outcome;
  }
  if (const auto value = get(result_key)) {
    outcome.result.emplace();
    outcome.result->reserve(value->size());
    value->append_to(*outcome.result);
  }
  return outcome;
}

std::optional<std::size_t> Store::strlen(std::string_view key) const noexcept {
  return keyspace_.string_length(key);
}

std::optional<std::size_t> Store::append(std::string_view key,
                                         std::string_view value) {
  const auto current = keyspace_.get_string(key);
  if (!current) {
    if (!value_fits(value)) {
      return std::nullopt;
    }
    keyspace_.set_string(key, value);
    signal_key_modified(key);
    return value.size();
  }
  std::string combined = join_value(*current);
  if (value.size() > kStringMaxCompressedLogicalBytes - combined.size()) {
    return std::nullopt;
  }
  combined.append(value);
  if (!value_fits(combined)) {
    return std::nullopt;
  }
  keyspace_.set_string(key, combined);
  signal_key_modified(key);
  return combined.size();
}

std::optional<long long> Store::incr_by(std::string_view key, long long delta) {
  long long current = 0;
  if (const auto value = keyspace_.get_string(key)) {
    const auto parsed = parse_i64(join_value(*value));
    if (!parsed) {
      return std::nullopt;
    }
    current = *parsed;
  }
  if ((delta > 0 && current > std::numeric_limits<long long>::max() - delta) ||
      (delta < 0 && current < std::numeric_limits<long long>::min() - delta)) {
    return std::nullopt;
  }
  const long long result = current + delta;
  keyspace_.set_string(key, std::to_string(result));
  signal_key_modified(key);
  return result;
}

std::optional<long long> Store::incr_bound(std::string_view key, long long delta,
                                           long long max) {
  long long current = 0;
  if (const auto value = keyspace_.get_string(key)) {
    const auto parsed = parse_i64(join_value(*value));
    if (!parsed) {
      return std::nullopt;  // not an integer
    }
    current = *parsed;
  }
  // Bound check without overflow: current and delta are int64, so their sum fits
  // in int128. `max` is an int64, so sum <= max guarantees the admitted result
  // fits the int64 high end; only the low end can underflow.
  const __int128 sum =
      static_cast<__int128>(current) + static_cast<__int128>(delta);
  if (sum > static_cast<__int128>(max)) {
    return -1;  // over budget -- reject; the key is left untouched
  }
  if (sum < static_cast<__int128>(std::numeric_limits<long long>::min())) {
    return std::nullopt;  // admitted result would underflow long long
  }
  const long long result = static_cast<long long>(sum);
  keyspace_.set_string(key, std::to_string(result));  // preserves any TTL
  signal_key_modified(key);
  return result;
}

std::optional<long long> Store::decr_positive(std::string_view key) {
  long long current = 0;
  if (const auto value = keyspace_.get_string(key)) {
    const auto parsed = parse_i64(join_value(*value));
    if (!parsed) {
      return std::nullopt;  // not an integer
    }
    current = *parsed;
  }
  if (current <= 0) {
    return -1;  // nothing to reserve -- reject; the key is never created/modified
  }
  const long long result = current - 1;  // current >= 1, so no underflow
  keyspace_.set_string(key, std::to_string(result));  // preserves any TTL
  signal_key_modified(key);
  return result;
}

std::optional<long long> Store::incr_expire(std::string_view key,
                                            std::uint64_t when_ms,
                                            std::uint64_t now) {
  const auto value = incr_by(key, 1);
  if (!value) {
    return std::nullopt;  // not an integer, or overflow
  }
  // A result of 1 means the key was just created (incr_by keeps any existing TTL
  // in place, so a running counter's window keeps ticking): arm the window.
  if (*value == 1) {
    (void)expire_at_ms(key, when_ms, now);
  }
  return value;
}

std::optional<std::string> Store::incr_by_float(std::string_view key,
                                                double delta) {
  double current = 0.0;
  if (const auto value = keyspace_.get_string(key)) {
    const auto parsed = parse_double(join_value(*value));
    if (!parsed) {
      return std::nullopt;
    }
    current = *parsed;
  }
  const double result = current + delta;
  if (!std::isfinite(result)) {
    return std::nullopt;
  }
  std::string text = format_incrbyfloat(result);
  keyspace_.set_string(key, text);
  signal_key_modified(key);
  return text;
}

std::string Store::getrange(std::string_view key, long long start,
                            long long end) const {
  const auto value = keyspace_.get_string(key);
  if (!value || value->size() == 0) {
    return {};
  }
  const auto len = static_cast<long long>(value->size());
  if (start < 0) {
    start += len;
  }
  if (end < 0) {
    end += len;
  }
  if (start < 0) {
    start = 0;
  }
  if (end < 0) {
    end = 0;
  }
  if (end >= len) {
    end = len - 1;
  }
  if (start > end) {  // start >= len also lands here, since end <= len - 1
    return {};
  }
  std::string joined = join_value(*value);
  return joined.substr(static_cast<std::size_t>(start),
                       static_cast<std::size_t>(end - start + 1));
}

std::optional<std::size_t> Store::setrange(std::string_view key,
                                           std::size_t offset,
                                           std::string_view value) {
  const auto current = keyspace_.get_string(key);
  const std::size_t current_len = current ? current->size() : 0;
  // An empty value never creates or grows: reply with the current length (0 when
  // the key is absent).
  if (value.empty()) {
    return current_len;
  }
  // The compressed form carries a 24-bit logical length. Check before adding so
  // the result-size arithmetic cannot overflow.
  if (offset > kStringMaxCompressedLogicalBytes ||
      value.size() > kStringMaxCompressedLogicalBytes - offset) {
    return std::nullopt;
  }
  const std::size_t result_len = std::max(current_len, offset + value.size());
  std::string buffer;
  buffer.reserve(result_len);
  if (current) {
    current->append_to(buffer);
  }
  buffer.resize(result_len, '\0');  // zero-pad any gap up to offset
  std::copy(value.begin(), value.end(),
            buffer.begin() + static_cast<std::ptrdiff_t>(offset));
  if (!value_fits(buffer)) {
    return std::nullopt;
  }
  keyspace_.set_string(key, buffer);
  signal_key_modified(key);
  return result_len;
}

// ---- TTL ----

std::uint64_t Store::now_ms() const noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool Store::expire_at_ms(std::string_view key, std::uint64_t when_ms,
                         std::uint64_t now, unsigned flags) {
  const auto id = keyspace_.id_of(key);
  if (!id) {
    return false;
  }
  const auto current = ttl_.expiry(*id);
  const bool has_current = current.has_value();
  // A key with no current expiry is +infinity for GT/LT.
  if ((flags & ExpireFlag::kNx) && has_current) {
    return false;
  }
  if ((flags & ExpireFlag::kXx) && !has_current) {
    return false;
  }
  if ((flags & ExpireFlag::kGt) && (!has_current || when_ms <= *current)) {
    return false;
  }
  if ((flags & ExpireFlag::kLt) && (has_current && when_ms >= *current)) {
    return false;
  }
  if (when_ms <= now) {  // condition met, but the time is already past -> delete
    ttl_.clear(*id);
    erase_keyspace_at(*id);
    return true;
  }
  ttl_.set(*id, when_ms);
  signal_key_modified(key);
  return true;
}

long long Store::pttl_ms(std::string_view key, std::uint64_t now) const {
  const auto id = keyspace_.id_of(key);
  if (!id) {
    return -2;
  }
  const auto expiry = ttl_.expiry(*id);
  if (!expiry) {
    return -1;
  }
  if (*expiry <= now) {
    return -2;  // due but not yet purged -- report as already gone
  }
  return static_cast<long long>(*expiry - now);
}

long long Store::expiretime_ms(std::string_view key) const {
  const auto id = keyspace_.id_of(key);
  if (!id) {
    return -2;
  }
  const auto expiry = ttl_.expiry(*id);
  if (!expiry) {
    return -1;
  }
  return static_cast<long long>(*expiry);
}

bool Store::persist(std::string_view key) {
  const auto id = keyspace_.id_of(key);
  if (!id) {
    return false;
  }
  const bool removed = ttl_.clear(*id);
  if (removed) {
    signal_key_modified(key);
  }
  return removed;
}

bool Store::purge_if_expired(std::string_view key, std::uint64_t now) {
  if (ttl_.empty()) {
    return false;
  }
  const auto id = keyspace_.id_of(key);
  if (!id) {
    return false;
  }
  const auto expiry = ttl_.expiry(*id);
  if (!expiry || *expiry > now) {
    return false;
  }
  ttl_.clear(*id);
  erase_keyspace_at(*id);
  return true;
}

std::size_t Store::active_expire(std::uint64_t now, std::size_t budget) {
  return ttl_.expire_due(now, budget,
                         [this](std::uint64_t id) { erase_keyspace_at(id); });
}

long long Store::zcard(std::string_view key) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }

  return static_cast<long long>(zset->size());
}

long long Store::zcount(std::string_view key, double min, bool min_exclusive,
                        double max, bool max_exclusive) const {
  const auto* zset = find_zset(key);
  if (zset == nullptr) {
    return 0;
  }
  const auto bounds =
      zset->score_range_bounds(min, min_exclusive, max, max_exclusive);
  return bounds ? static_cast<long long>(bounds->count) : 0;
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

  return zset->memory_stats(zset_options());
}

std::optional<std::size_t> Store::optimize(std::string_view key,
                                           double member_index_density) {
  if (auto* zset = find_zset(key); zset != nullptr) {
    const auto before = zset->memory_stats(zset_options()).total_allocated_bytes;
    zset->compact(member_index_density);
    const auto after = zset->memory_stats(zset_options()).total_allocated_bytes;
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
  if (auto* list = find_list(key); list != nullptr) {
    const auto before = list->memory_stats().total_allocated_bytes;
    list->compact();
    release_unused_heap_pages();
    const auto after = list->memory_stats().total_allocated_bytes;
    return before > after ? before - after : 0;
  }
  if (auto* set = find_set(key); set != nullptr) {
    const auto before = set->memory_stats().total_allocated_bytes;
    set->compact(member_index_density);
    release_unused_heap_pages();
    const auto after = set->memory_stats().total_allocated_bytes;
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

void ZSet::save(snapshot::Writer& writer, bool with_accelerator,
                const ZSetOptions* options) const {
  writer.u8(static_cast<std::uint8_t>(options->rank_cache_mode));
  writer.u8(options->score_string_cache ? 1 : 0);
  writer.f64(options->member_index_growth);

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

  const auto member_count = static_cast<std::uint32_t>(reader.u64());
  struct LoadedMember {
    double score;
    std::string member;
  };
  std::vector<LoadedMember> loaded;
  loaded.reserve(member_count);
  for (std::uint32_t id = 0; id < member_count; ++id) {
    loaded.push_back(LoadedMember{read_zset_score(reader, width),
                                 std::string(reader.str())});
  }

  const bool accelerator_present = reader.u8() != 0;
  const bool can_keep_listpack =
      !accelerator_present && options->listpack_max_entries > 0 &&
      !options->score_string_cache &&
      member_count <= options->listpack_max_entries;
  if (can_keep_listpack) {
    CompactListpack lp;
    bool fits = true;
    for (const auto& entry : loaded) {
      const auto result =
          lp.add(entry.score, entry.member, options->listpack_max_entries);
      if (result.needs_full) {
        fits = false;
        break;
      }
    }
    if (fits) {
      ZSet zset(options);
      zset.rep_ = std::move(lp);
      return zset;
    }
  }

  ZSet zset(options);
  zset.ensure_full(options);
  zset.member_storage()->reserve(member_count);

  // Canonical rebuild: push_back assigns member ids 0..N-1 in the same order
  // they were written, so accelerator member ids stay valid. Scores are read at
  // the persisted width; push_back re-derives the narrowest width that fits (so a
  // set saved wide but now all-integer loads back narrow). Dead bytes are dropped.
  for (const auto& entry : loaded) {
    (void)zset.member_storage()->push_back(entry.member, entry.score);
  }

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
  // File header, then one section per data family.
  std::string header;
  snapshot::Writer writer(header);
  writer.bytes(snapshot::kMagic, sizeof(snapshot::kMagic));
  writer.u32(snapshot::kFormatVersion);
  writer.u32(0);  // file flags (reserved)
  writer.u32(7);  // section count (ZSET, HASH, LIST, SET, ARRAY, STRING, TTL)
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
  const ZSetOptions* zopts = zset_options();
  auto emit_zset = [&out, &operands, with_accelerator, zopts](
                       std::string_view key, const ZSet& zset) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    zset.save(operand_writer, with_accelerator, zopts);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::ZsetOpcode::Zset));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };
  keyspace_.for_each_zset(emit_zset);

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
  keyspace_.for_each_hash(emit_hash);
  out.write(&end, 1);

  // LIST section: canonical order is always persisted. The optional accelerator
  // marks all-raw lists whose canonical bytes are already their final payload;
  // PMA slots, density, and arena addresses remain implementation details.
  std::string list_header;
  snapshot::Writer list_writer(list_header);
  list_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::List));
  list_writer.u32(with_accelerator ? snapshot::kListAcceleratorVersion : 0);
  list_writer.u64(0);
  out.write(list_header.data(), static_cast<std::streamsize>(list_header.size()));

  auto emit_list = [&out, &operands, with_accelerator](
                       std::string_view key, const List& list) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    operand_writer.u64(static_cast<std::uint64_t>(list.size()));
    bool all_raw = true;
    list.for_each([&](EncodedStringView value) {
      operand_writer.u32(static_cast<std::uint32_t>(value.size()));
      value.append_to(operands);
      all_raw = all_raw && value.is_raw();
    });

    if (with_accelerator) {
      operand_writer.u8(all_raw ? 1 : 0);
      if (all_raw) {
        operand_writer.u8(
            list.options().string_encoding.encoding_enabled() ? 1 : 0);
      }
    }

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::ListOpcode::List));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };
  keyspace_.for_each_list(emit_list);
  out.write(&end, 1);

  // SET section: canonical logical members in dense id order, optional Swiss
  // accelerator (same hash identity as zset/hash member indexes). Encoding is
  // the loader's; the accelerator carries an encoding-identity word so a
  // reclassify mismatch falls back to rebuild.
  std::string set_header;
  snapshot::Writer set_writer(set_header);
  set_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Set));
  set_writer.u32(with_accelerator ? snapshot::kSetAcceleratorVersion : 0);
  set_writer.u64(ZSetMemberIndex::hash_identity());
  out.write(set_header.data(), static_cast<std::streamsize>(set_header.size()));

  auto emit_set = [&out, &operands, with_accelerator](std::string_view key,
                                                      const Set& set) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    set.save(operand_writer, with_accelerator);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::SetOpcode::Set));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(),
              static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };
  keyspace_.for_each_set(emit_set);
  out.write(&end, 1);

  // ARRAY section: canonical (index, logical value) pairs + geometry /
  // implementation. Leaf tables rebuild on load (no accelerator yet).
  std::string array_header;
  snapshot::Writer array_writer(array_header);
  array_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Array));
  array_writer.u32(snapshot::kArrayAcceleratorVersion);
  array_writer.u64(0);
  out.write(array_header.data(),
            static_cast<std::streamsize>(array_header.size()));

  auto emit_array = [&out, &operands](std::string_view key, const Array& array) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    array.save(operand_writer);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(
        static_cast<std::uint8_t>(snapshot::ArrayOpcode::Array));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(),
              static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };
  keyspace_.for_each_array(emit_array);
  out.write(&end, 1);

  // STRING section stays canonical logical bytes; the in-memory value encoding
  // is rebuilt on load and never leaks into the portable snapshot.
  std::string string_header;
  snapshot::Writer string_writer(string_header);
  string_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::String));
  string_writer.u32(0);
  string_writer.u64(0);
  out.write(string_header.data(),
            static_cast<std::streamsize>(string_header.size()));

  std::string value_scratch;
  auto emit_string = [&out, &operands, &value_scratch](std::string_view key,
                                                       StringValueView value) {
    value_scratch.clear();
    value_scratch.reserve(value.size());
    value.append_to(value_scratch);

    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(key);
    operand_writer.str(value_scratch);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(
        static_cast<std::uint8_t>(snapshot::StringOpcode::String));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  };
  keyspace_.for_each_string(emit_string);
  out.write(&end, 1);

  // TTL section: each key's absolute expiry ms, its own object family. Written
  // last so a loader has already placed every key before it resolves ids to
  // attach expiries. No accelerator.
  std::string ttl_header;
  snapshot::Writer ttl_writer(ttl_header);
  ttl_writer.u32(static_cast<std::uint32_t>(snapshot::SectionType::Ttl));
  ttl_writer.u32(0);
  ttl_writer.u64(0);
  out.write(ttl_header.data(), static_cast<std::streamsize>(ttl_header.size()));

  ttl_.for_each([&](std::uint64_t key_id, std::uint64_t expiry_ms) {
    operands.clear();
    snapshot::Writer operand_writer(operands);
    operand_writer.str(keyspace_.key_for_id(key_id));
    operand_writer.u64(expiry_ms);

    std::string instruction;
    snapshot::Writer instruction_writer(instruction);
    instruction_writer.u8(static_cast<std::uint8_t>(snapshot::TtlOpcode::Ttl));
    instruction_writer.u64(operands.size());
    instruction_writer.u32(snapshot::checksum(operands));
    out.write(instruction.data(), static_cast<std::streamsize>(instruction.size()));
    out.write(operands.data(), static_cast<std::streamsize>(operands.size()));
  });
  out.write(&end, 1);

  if (!out) {
    throw snapshot::snapshot_error("snapshot write failed");
  }
}

bool Store::save_to_file(const std::string& path,
                         bool with_accelerator) const noexcept {
  // Write to a temp file, fsync, and atomically rename into place. Used by both the
  // forked child and the synchronous (huge-page) save; on the child path the caller
  // _exit()s the result without running the parent's destructors.
  const std::string temp = path + ".tmp";
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return false;
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
  return ok;
}

Store::SaveStart Store::start_background_save(std::string path,
                                             bool with_accelerator) {
  if (background_save_child_ > 0 || background_save_sync_pending_) {
    return SaveStart::AlreadyRunning;
  }

  if (hugetlb::arena_enabled()) {
    // Huge pages make fork+COW unsafe: a huge-page mapping copies-on-write at 2 MiB
    // granularity, so one post-fork write duplicates a whole 2 MiB page and pulls a
    // fresh huge page from the small fixed pool -- amplifying RSS and SIGBUS-ing the
    // parent once the pool exhausts. Save synchronously in-process instead: the event
    // loop blocks for the duration, but nothing is written after a fork so nothing
    // copies-on-write. reap_background_save() returns the outcome on its next call.
    background_save_sync_ok_ = save_to_file(path, with_accelerator);
    background_save_path_ = std::move(path);
    background_save_sync_pending_ = true;
    return SaveStart::Started;
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return SaveStart::ForkFailed;
  }

  if (child == 0) {
    // Child: a copy-on-write snapshot of the store, frozen at fork time. Write it and
    // _exit without running the parent's destructors or atexit handlers (they belong
    // to the parent); inherited descriptors are left untouched and close on _exit.
    _exit(save_to_file(path, with_accelerator) ? 0 : 1);
  }

  background_save_child_ = child;
  background_save_path_ = std::move(path);
  return SaveStart::Started;
}

std::optional<Store::SaveOutcome> Store::reap_background_save() noexcept {
  if (background_save_sync_pending_) {
    // The synchronous (huge-page) save already finished inside start_background_save.
    background_save_sync_pending_ = false;
    SaveOutcome outcome{.path = std::move(background_save_path_),
                        .ok = background_save_sync_ok_};
    background_save_path_.clear();
    return outcome;
  }
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
  // A load replaces the complete keyspace. Active WATCH clients must abort even
  // when a watched key is absent from both the old and new snapshots.
  signal_all_modified();
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
  signal_all_modified();
  keyspace_.clear();
  ttl_.clear_all();
}

SnapshotLoadStats Store::load_native(std::istream& in) {
  keyspace_.clear();
  ttl_.clear_all();

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
    const auto format_version = header_reader.u32();
    if (format_version < snapshot::kOldestReadableFormatVersion ||
        format_version > snapshot::kFormatVersion) {
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
      const bool is_string =
          section_type ==
          static_cast<std::uint32_t>(snapshot::SectionType::String);
      const bool is_list =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::List);
      const bool is_set =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::Set);
      const bool is_array =
          section_type ==
          static_cast<std::uint32_t>(snapshot::SectionType::Array);
      const bool is_ttl =
          section_type == static_cast<std::uint32_t>(snapshot::SectionType::Ttl);
      const bool zset_use_accelerator =
          is_zset && section_version == snapshot::kZsetAcceleratorVersion &&
          hash_identity == ZSetMemberIndex::hash_identity();
      const bool hash_use_accelerator =
          is_hash && section_version == snapshot::kHashAcceleratorVersion &&
          hash_identity == ZSetMemberIndex::hash_identity();
      const bool list_use_accelerator =
          is_list && section_version == snapshot::kListAcceleratorVersion;
      const bool set_use_accelerator =
          is_set && section_version == snapshot::kSetAcceleratorVersion &&
          hash_identity == ZSetMemberIndex::hash_identity();
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
          stats.used_accelerator =
              stats.used_accelerator || zset_use_accelerator;
          stats.members += zset.size();
          place_loaded_zset(std::move(key), std::move(zset));
          ++stats.keys;
        } else if (is_hash && opcode == static_cast<std::uint8_t>(
                                            snapshot::HashOpcode::Hash)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          Hash hash =
              Hash::load(reader, hash_use_accelerator,
                         HashOptions{
                             .member_index_growth = options_.member_index_growth,
                             .chunk_bytes = options_.hash_chunk_bytes,
                             .compaction_knapsack =
                                 options_.hash_compaction_knapsack,
                             .string_encoding = options_.string_encoding,
                             .compaction_work_budget =
                                 options_.hash_compaction_work_budget,
                             .listpack_max_entries =
                                 options_.hash_listpack_max_entries,
                             .realtime_index_bytes =
                                 options_.realtime_hash_index_bytes,
                         },
                         options_.real_time ? HashImplementation::Realtime
                                            : options_.hash_implementation,
                         format_version >= 3);
          stats.used_accelerator =
              stats.used_accelerator ||
              (hash_use_accelerator &&
               hash.implementation() == HashImplementation::Efficient);
          stats.members += hash.size();
          place_loaded_hash(std::move(key), std::move(hash));
          ++stats.keys;
        } else if (is_string &&
                   opcode == static_cast<std::uint8_t>(
                                 snapshot::StringOpcode::String)) {
          snapshot::Reader reader(operands);
          const auto key = reader.str();
          const auto value = reader.str();
          keyspace_.set_string(key, value);
          ++stats.keys;
        } else if (is_list && opcode == static_cast<std::uint8_t>(
                                                snapshot::ListOpcode::List)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          const auto count = reader.u64();
          if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw snapshot::snapshot_error("snapshot list is too large");
          }
          std::vector<std::string_view> values;
          values.reserve(static_cast<std::size_t>(count));
          for (std::uint64_t index = 0; index < count; ++index) {
            values.push_back(reader.str());
          }
          List list(build_list_options(options_));
          bool accelerated = false;
          if (list_use_accelerator && reader.u8() != 0) {
            const bool encoding_enabled = reader.u8() != 0;
            if (encoding_enabled ==
                list.options().string_encoding.encoding_enabled()) {
              list.assign_raw(values);
              accelerated = true;
            }
          }
          if (!accelerated) {
            list.assign(values);
          }
          stats.used_accelerator = stats.used_accelerator || accelerated;
          stats.members += static_cast<std::size_t>(count);
          place_loaded_list(std::move(key), std::move(list));
          ++stats.keys;
        } else if (is_set && opcode == static_cast<std::uint8_t>(
                                           snapshot::SetOpcode::Set)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          Set set = Set::load(reader, set_use_accelerator,
                              build_set_options(options_));
          stats.used_accelerator =
              stats.used_accelerator || set_use_accelerator;
          stats.members += set.size();
          place_loaded_set(std::move(key), std::move(set));
          ++stats.keys;
        } else if (is_array && opcode == static_cast<std::uint8_t>(
                                             snapshot::ArrayOpcode::Array)) {
          snapshot::Reader reader(operands);
          auto key = std::string(reader.str());
          Array array = Array::load(reader, build_array_options(options_));
          stats.members += array.count();
          place_loaded_array(std::move(key), std::move(array));
          ++stats.keys;
        } else if (is_ttl && opcode == static_cast<std::uint8_t>(
                                           snapshot::TtlOpcode::Ttl)) {
          snapshot::Reader reader(operands);
          const auto key = reader.str();
          const auto expiry_ms = reader.u64();
          if (const auto id = keyspace_.id_of(key)) {
            if (expiry_ms <= now_ms()) {
              // Already past at load time: drop the key, as Redis does, rather
              // than load it and lean on lazy/active expiration. (The TTL section
              // is last, so the key exists; erasing by id fixes up the TTL of the
              // key the swap-remove slides into its slot.)
              erase_keyspace_at(*id);
              --stats.keys;
            } else {
              ttl_.set(*id, expiry_ms);
            }
          }
        }
        // else: unknown opcode or section -- already consumed, skip.
      }
    }
    return stats;
  } catch (...) {
    keyspace_.clear();
    ttl_.clear_all();
    throw;
  }
}

void Store::place_loaded_zset(std::string key, ZSet&& zset) {
  (void)keyspace_.place_loaded_zset(key, std::move(zset));
}

StoreMemoryStats Store::memory_stats() const noexcept {
  StoreMemoryStats stats;
  const ZSetOptions* zopts = zset_options();
  keyspace_.for_each_zset([&stats, zopts](std::string_view, const ZSet& zset) {
    ++stats.overflow_zset_count;
    stats.overflow_zset_allocated_bytes +=
        zset.memory_stats(zopts).total_allocated_bytes;
  });
  // The unified keyspace no longer splits zsets into inline/overflow tables; the
  // arena + index + object/type table overhead lands in one figure.
  stats.overflow_zset_capacity = keyspace_.size();
  stats.overflow_table_allocated_bytes = keyspace_.allocated_bytes();
  stats.total_allocated_bytes =
      stats.overflow_table_allocated_bytes + stats.overflow_zset_allocated_bytes;
  return stats;
}

MemoryReport Store::memory_report() const noexcept {
  MemoryReport r;
  r.realtime_hash_index_pool_bytes =
      keyspace_.realtime_hash_index_pool_bytes();
  r.realtime_keyspace_index_pool_bytes =
      keyspace_.realtime_keyspace_index_pool_bytes();
  const ZSetOptions* zopts = zset_options();
  // Full zsets: arena used (live + dead) plus fixed index/cache structures.
  // Compact zset blobs are accounted once through the process-wide pool below.
  keyspace_.for_each_zset([&r, zopts](std::string_view, const ZSet& zset) {
    const auto s = zset.memory_stats(zopts);
    if (!zset.is_small()) {
      r.used_memory +=
          s.member_storage_bytes + s.score_string_cache_bytes +
          s.member_index_allocated_bytes + s.score_index_allocated_bytes +
          s.rank_location_cache_allocated_bytes;
    }
    r.reclaimable_bytes += s.member_storage_dead_bytes;
  });
  // Full hashes own heap state and a field-value arena. Compact hash blobs live
  // in KeyspaceStorage and are therefore already included in its footprint.
  keyspace_.for_each_hash([&r](std::string_view, const Hash& hash) {
    const auto s = hash.memory_stats();
    const auto hash_owned_bytes =
        s.field_value_live_bytes + s.field_value_dead_bytes +
        s.field_index_allocated_bytes + s.hash_heap_allocated_bytes;
    r.used_memory += hash_owned_bytes - s.keyspace_accounted_bytes;
    r.reclaimable_bytes += s.field_value_dead_bytes;
    r.hash_heap_allocated_bytes += s.hash_heap_allocated_bytes;
  });
  // PMA lists own a value arena and PMA arrays. Small-list and segmented-leaf
  // blobs are covered by the process-wide pool/upstream capacity below, so only
  // the segmented leaf-index vectors are added here.
  keyspace_.for_each_list([&r](std::string_view, const List& list) {
    const auto s = list.memory_stats();
    r.used_memory += s.object_allocated_bytes;
    if (!list.is_small()) {
      r.used_memory += s.order_allocated_bytes;
      if (list.implementation() == ListImplementation::Pma) {
        r.used_memory += s.value_allocated_bytes;
      }
    }
    r.reclaimable_bytes += s.value_dead_bytes;
  });
  // Sets own an encoded-member arena and a Swiss member index.
  keyspace_.for_each_set([&r](std::string_view, const Set& set) {
    const auto s = set.memory_stats();
    r.used_memory += s.total_allocated_bytes;
    r.reclaimable_bytes += s.member_dead_bytes;
  });
  // The keyspace itself: key/value arena (live + dead) plus the swiss index and
  // object/type tables.
  r.used_memory += keyspace_.footprint_used_bytes();
  r.reclaimable_bytes += keyspace_.footprint_dead_bytes();

  const auto pool = blob_pool_stats();
  r.blob_pool_requested_bytes = pool.requested_live_bytes;
  r.blob_pool_capacity_bytes = pool.upstream_capacity_bytes;
  r.blob_pool_fragmentation_bytes = pool.overhead_bytes();
  r.blob_pool_live_allocations = pool.live_allocations;
  r.blob_pool_upstream_allocations = pool.upstream_allocations;
  r.used_memory += pool.upstream_capacity_bytes;
  return r;
}

std::string format_score(double score) {
  return score_format::format(score);
}

}  // namespace goblin::core
