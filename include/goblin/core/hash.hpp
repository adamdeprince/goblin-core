#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "goblin/core/hash_storage.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/zset_member_index.hpp"

namespace goblin::core {

// A hash uses only the growth knob; there is no score, rank cache, or score-text
// cache to tune (it maps field->value, unordered).
struct HashOptions {
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t chunk_bytes{HashStorage::kDefaultChunkBytes};
};

struct HashMemoryStats {
  std::size_t field_count{0};
  std::size_t field_value_live_bytes{0};
  std::size_t field_value_dead_bytes{0};
  std::size_t field_value_allocated_bytes{0};
  std::size_t field_index_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

// A Redis hash: field->value, both arbitrary byte strings (<= 64 KiB each). The
// field->id lookup is the same tuned Swiss table the zset uses (MemberIndex);
// field and value bytes are packed in HashStorage. No ordering is kept.
class Hash {
 public:
  static constexpr double kDefaultFieldIndexDensity = 0.97;

  explicit Hash(HashOptions options = {})
      : storage_(std::make_unique<HashStorage>(options.chunk_bytes,
                                               options.member_index_growth)),
        fields_(storage_.get(), options.member_index_growth),
        options_(options) {}

  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;

  Hash(Hash&& other) noexcept
      : storage_(std::move(other.storage_)),
        fields_(std::move(other.fields_)),
        options_(other.options_) {
    fields_.set_members(storage_.get());
  }
  Hash& operator=(Hash&& other) noexcept {
    if (this != &other) {
      storage_ = std::move(other.storage_);
      fields_ = std::move(other.fields_);
      options_ = other.options_;
      fields_.set_members(storage_.get());
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return storage_->size(); }
  [[nodiscard]] bool empty() const noexcept { return storage_->empty(); }
  [[nodiscard]] const HashOptions& options() const noexcept { return options_; }

  // HSET one field. Returns 1 if the field is new, 0 if it updated an existing
  // field's value.
  int set(std::string_view field, std::string_view value) {
    if (auto* meta = fields_.find(field); meta != nullptr) {
      storage_->set_value(meta->member_id, value);
      maybe_compact();
      return 0;
    }
    insert_new(field, value);
    return 1;
  }

  // HSETNX. Returns 1 if the field was set, 0 if it already existed.
  int set_nx(std::string_view field, std::string_view value) {
    if (fields_.find(field) != nullptr) {
      return 0;
    }
    insert_new(field, value);
    return 1;
  }

  [[nodiscard]] std::optional<std::string_view> get(std::string_view field) const {
    const auto* meta = fields_.find(field);
    if (meta == nullptr) {
      return std::nullopt;
    }
    return storage_->value(meta->member_id);
  }

  [[nodiscard]] bool contains(std::string_view field) const {
    return fields_.find(field) != nullptr;
  }

  // HDEL one field. Returns true if it was present and removed.
  bool erase(std::string_view field) {
    auto* meta = fields_.find(field);
    if (meta == nullptr) {
      return false;
    }
    const auto field_id = meta->member_id;
    storage_->orphan(field_id);
    const bool erased = fields_.erase(field);
    assert(erased);
    move_last_field_into_slot(field_id);
    maybe_compact();
    return erased;
  }

  // Iterate every (field, value) in id order. fn(std::string_view field,
  // std::string_view value).
  template <class Fn>
  void for_each(Fn&& fn) const {
    const auto n = static_cast<std::uint32_t>(storage_->size());
    for (std::uint32_t id = 0; id < n; ++id) {
      fn(storage_->view(id), storage_->value(id));
    }
  }

  // Rebuild: drop orphaned (dead) blob bytes and repack the field index to the
  // target density. Called on GOBLIN.OPTIMIZE and automatically when
  // fragmentation exceeds the live bytes (see maybe_compact).
  void compact(double field_index_density = kDefaultFieldIndexDensity) {
    const auto n = static_cast<std::uint32_t>(storage_->size());
    auto new_storage = std::make_unique<HashStorage>(
        storage_->chunk_bytes(), options_.member_index_growth);
    new_storage->reserve(n);
    MemberIndex<HashStorage> new_index(new_storage.get(),
                                       options_.member_index_growth);
    new_index.reserve_for_density(n, field_index_density);
    for (std::uint32_t id = 0; id < n; ++id) {
      const auto new_id =
          new_storage->push_back(storage_->view(id), storage_->value(id));
      new_index.insert_packed(new_storage->view(new_id),
                              ZSetMemberMeta{.member_id = new_id});
    }
    storage_ = std::move(new_storage);
    fields_ = std::move(new_index);
    fields_.set_members(storage_.get());
  }

  [[nodiscard]] HashMemoryStats memory_stats() const noexcept {
    HashMemoryStats stats;
    stats.field_count = storage_->size();
    stats.field_value_live_bytes = storage_->live_bytes();
    stats.field_value_dead_bytes = storage_->dead_bytes();
    stats.field_value_allocated_bytes = storage_->allocated_bytes();
    stats.field_index_allocated_bytes = fields_.allocated_bytes();
    stats.total_allocated_bytes =
        stats.field_value_allocated_bytes + stats.field_index_allocated_bytes;
    return stats;
  }

  [[nodiscard]] std::size_t field_index_capacity() const noexcept {
    return fields_.capacity();
  }

  // Snapshot. Canonical layer: (field, value) in id order -- the portable
  // "unpacked table" that rebuilds under any tuning. Optional accelerator: the
  // raw swiss field-index dump, version- and hash_identity-gated at the section
  // header (see snapshot.hpp / Store::save). Unlike the zset there is no
  // score-order fallback -- the canonical layer is the only durable copy of the
  // table, so it is always written.
  void save(snapshot::Writer& writer, bool with_accelerator) const {
    writer.f64(options_.member_index_growth);
    const auto field_count = static_cast<std::uint32_t>(storage_->size());
    writer.u64(field_count);
    for (std::uint32_t id = 0; id < field_count; ++id) {
      writer.str(storage_->view(id));
      writer.str(storage_->value(id));
    }
    writer.u8(with_accelerator ? 1 : 0);
    if (with_accelerator) {
      fields_.write_accelerator(writer);
    }
  }

  [[nodiscard]] static Hash load(
      snapshot::Reader& reader, bool use_accelerator,
      std::size_t chunk_bytes = HashStorage::kDefaultChunkBytes) {
    HashOptions options;
    options.member_index_growth = reader.f64();
    // Chunk size is a server-level layout tuning, not persisted per hash.
    options.chunk_bytes = chunk_bytes;
    Hash hash(options);

    const auto field_count = static_cast<std::uint32_t>(reader.u64());
    hash.storage_->reserve(field_count);
    for (std::uint32_t id = 0; id < field_count; ++id) {
      const auto field = reader.str();
      const auto value = reader.str();
      (void)hash.storage_->push_back(field, value);
    }

    const bool accelerator_present = reader.u8() != 0;
    if (accelerator_present && use_accelerator) {
      // Trust the dumped table (version + hash_identity already validated).
      hash.fields_.read_accelerator(reader, hash.storage_.get());
    } else {
      // Unpack the canonical table into a fresh index at the persisted growth.
      // Any accelerator bytes present stay unread in the operand buffer.
      hash.fields_.reserve_for_density(field_count, kDefaultFieldIndexDensity);
      for (std::uint32_t id = 0; id < field_count; ++id) {
        hash.fields_.insert_packed(hash.storage_->view(id),
                                   ZSetMemberMeta{.member_id = id});
      }
    }
    return hash;
  }

 private:
  void insert_new(std::string_view field, std::string_view value) {
    const auto field_id = storage_->push_back(field, value);
    fields_.insert(storage_->view(field_id),
                   ZSetMemberMeta{.member_id = field_id});
  }

  // Keep field ids dense (0..N-1): move the last field into the removed slot so
  // the storage and the accelerator dump stay contiguous.
  void move_last_field_into_slot(std::uint32_t removed_field_id) {
    const auto last_id = static_cast<std::uint32_t>(storage_->size() - 1);
    if (removed_field_id == last_id) {
      storage_->pop_back();
      return;
    }
    storage_->copy_ref(removed_field_id, last_id);
    const bool moved = fields_.move_member_id(last_id, removed_field_id);
    assert(moved);
    (void)moved;
    storage_->pop_back();
  }

  // Bound fragmentation without a manual OPTIMIZE: once at least a chunk's worth
  // of blob bytes is dead and dead has caught up to live, rebuild. Small hashes
  // never trip the floor, so they never churn.
  void maybe_compact() {
    if (storage_->dead_bytes() >= kAutoCompactDeadFloor &&
        storage_->dead_bytes() >= storage_->live_bytes()) {
      compact();
    }
  }

  static constexpr std::size_t kAutoCompactDeadFloor = std::size_t{1} << 20;

  std::unique_ptr<HashStorage> storage_;
  MemberIndex<HashStorage> fields_;
  HashOptions options_;
};

}  // namespace goblin::core
