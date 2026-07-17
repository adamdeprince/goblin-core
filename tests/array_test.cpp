// Unit tests for dual array implementations: Classic (Redis 8.8-style) and
// Realtime (fixed dense leaves, fixed geometry, fail closed). Snapshot save/load.

#include "goblin/core/array.hpp"
#include "goblin/core/command.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/store.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using goblin::core::Array;
using goblin::core::ArrayImplementation;
using goblin::core::ArrayOptions;
using goblin::core::ArrayStorage;
using goblin::core::Store;
using goblin::core::StoreOptions;

std::string execute_fields(
    Store& store, std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  const auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  return goblin::core::execute_command(store, *parsed.command);
}

int main() {
  // Sparse holes do not force intermediate leaf allocation.
  {
    Array a;
    assert(a.implementation() == ArrayImplementation::Classic);
    assert(a.set(0, "a"));
    assert(a.set(1'000'000, "b"));
    assert(a.count() == 2);
    assert(a.length() == 1'000'001);
    assert(a.get(0) == "a");
    assert(a.get(1'000'000) == "b");
    assert(!a.get(42).has_value());
  }

  // Classic: contiguous fill promotes sparse → dense.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Classic;
    options.slice_slots = 64;
    options.sparse_promote_load = 0.25;
    Array a(options);
    for (std::uint64_t i = 0; i < 20; ++i) {
      (void)a.set(i, std::to_string(i));
    }
    const auto stats = a.memory_stats();
    assert(stats.dense_slices >= 1);
    assert(a.get(19) == "19");
  }

  // Classic: extending a dense leaf into reserved tail capacity preserves
  // empty gaps and creates only the requested element.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Classic;
    options.slice_slots = 64;
    options.sparse_promote_load = 0.25;
    Array a(options);
    for (std::uint64_t i = 0; i < 18; ++i) {
      (void)a.set(i, std::to_string(i));
    }
    assert(a.memory_stats().dense_slices == 1);
    assert(a.set(21, "twenty-one"));
    assert(a.count() == 19);
    assert(!a.get(18).has_value());
    assert(!a.get(19).has_value());
    assert(!a.get(20).has_value());
    assert(a.get(21) == "twenty-one");
  }

  // Classic: auto directory depth promotion.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Classic;
    options.slice_slots = 4;
    options.dir_fanout = 4;
    options.initial_depth = 1;
    Array a(options);
    assert(a.depth() == 1);
    (void)a.set(15, "edge");
    assert(a.depth() == 1);
    (void)a.set(16, "promoted");
    assert(a.depth() >= 2);
    assert(a.get(15) == "edge");
    assert(a.get(16) == "promoted");
  }

  // RT: fixed depth — write past capacity errors.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 4;
    options.dir_fanout = 4;
    options.initial_depth = 1;
    Array a(options);
    assert(a.is_realtime());
    assert(a.depth() == 1);
    (void)a.set(15, "edge");
    assert(a.depth() == 1);
    bool threw = false;
    try {
      (void)a.set(16, "nope");
    } catch (const std::length_error&) {
      threw = true;
    }
    assert(threw);
    assert(a.depth() == 1);
    assert(a.get(15) == "edge");
    assert(!a.get(16).has_value());
  }

  // RT: a leaf allocates its final dense table once and never geometrically
  // reallocates while serving writes.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 64;
    options.sparse_promote_load = 0.01;
    Array a(options);
    for (std::uint64_t i = 0; i < 40; ++i) {
      (void)a.set(i, std::to_string(i));
    }
    assert(a.memory_stats().dense_slices == 1);
    assert(a.memory_stats().sparse_slices == 0);
    assert(a.memory_stats().leaf_table_bytes ==
           goblin::core::page_block_alloc_bytes(
               options.slice_slots * sizeof(Array::value_id)));
    assert(a.get(39) == "39");
  }

  // RT reservation faults all leaves/value storage before serving and then
  // fails closed at each configured boundary.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 16;
    options.dir_fanout = 16;
    options.initial_depth = 1;
    options.chunk_bytes = ArrayStorage::kMinChunkBytes;
    Array a(options);
    a.reserve_realtime(/*max_index=*/31, /*value_capacity=*/4,
                       /*value_bytes=*/64);
    const auto reserved = a.memory_stats();
    assert(reserved.realtime_reserved);
    assert(reserved.reserved_max_index == 31);
    assert(reserved.reserved_value_capacity == 4);
    assert(reserved.reserved_value_bytes == 64);
    assert(reserved.dense_slices == 2);
    assert(reserved.sparse_slices == 0);

    assert(a.set(0, "a"));
    assert(a.set(1, "b"));
    assert(a.set(2, "c"));
    assert(a.set(31, "d"));
    assert(!a.set(31, "updated"));
    assert(a.get(31) == "updated");
    bool value_exhausted = false;
    try {
      (void)a.set(3, "e");
    } catch (const std::length_error&) {
      value_exhausted = true;
    }
    assert(value_exhausted);
    bool index_exhausted = false;
    try {
      (void)a.set(32, "outside");
    } catch (const std::length_error&) {
      index_exhausted = true;
    }
    assert(index_exhausted);
  }

  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 16;
    options.chunk_bytes = ArrayStorage::kMinChunkBytes;
    Array a(options);
    a.reserve_realtime(/*max_index=*/15, /*value_capacity=*/8,
                       /*value_bytes=*/4);
    assert(a.set(0, "abc"));  // raw-string marker + three bytes
    bool bytes_exhausted = false;
    try {
      (void)a.set(0, "x");
    } catch (const std::length_error&) {
      bytes_exhausted = true;
    }
    assert(bytes_exhausted);
    assert(a.get(0) == "abc");
  }

  // A loaded/unreserved RT array can be rebuilt into a reservation without
  // losing values or its insert cursor.
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 16;
    options.chunk_bytes = ArrayStorage::kMinChunkBytes;
    Array a(options);
    assert(a.set(3, "existing"));
    assert(a.seek(9));
    a.reserve_realtime(/*max_index=*/15, /*value_capacity=*/3,
                       /*value_bytes=*/64);
    assert(a.realtime_reserved());
    assert(a.get(3) == "existing");
    assert(a.next_insert() == 9);
    assert(a.insert("next") == 9);
  }

  // Delete + insert cursor.
  {
    Array a;
    assert(a.insert("x") == 0);
    assert(a.insert("y") == 1);
    assert(a.next_insert() == 2);
    assert(a.seek(10));
    assert(a.insert("z") == 10);
    assert(a.del(1));
    assert(a.count() == 2);
    assert(!a.get(1).has_value());
  }

  // Compact integer encoding + arena stats.
  {
    Array a;
    (void)a.set(0, "42");
    assert(a.get(0) == "42");
    (void)a.set(1, "not-an-int-really-long-string-xxxxxxxx");
    assert(a.get(1)->size() > 10);
    assert(a.memory_stats().value_live_bytes > 0);
  }

  // Freelist reuse + id-stable compact.
  {
    Array a;
    for (int i = 0; i < 100; ++i) {
      (void)a.set(0, std::to_string(i));
    }
    assert(a.count() == 1);
    assert(a.get(0) == "99");
    assert(a.storage().live_count() == 1);
    assert(a.del(0));
    assert(a.storage().live_count() == 0);
  }

  {
    Array a;
    constexpr std::uint64_t kN = 64;
    for (std::uint64_t i = 0; i < kN; ++i) {
      (void)a.set(i, "v" + std::to_string(i));
    }
    for (int round = 0; round < 40; ++round) {
      for (std::uint64_t i = 0; i < kN; ++i) {
        (void)a.set(i, "r" + std::to_string(round) + "-" + std::to_string(i));
      }
    }
    (void)a.set(7, "");
    assert(a.del(11));
    a.compact_storage();
    assert(a.storage().dead_bytes() == 0);
    assert(a.get(7) == "");
    assert(!a.get(11).has_value());
    assert(a.get(0) == "r39-0");
    (void)a.set(11, "restored");
    a.compact_storage();
    assert(a.get(11) == "restored");
  }

  // kEmptyId reserved.
  static_assert(ArrayStorage::kEmptyId == 0xFFFFFFFFu);

  // Sparse entry packing.
  static_assert(sizeof(goblin::core::ArraySparseEntry) == 6);

  // Array::save / Array::load canonical round-trip (Classic + RT).
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Classic;
    options.slice_slots = 16;
    options.dir_fanout = 16;
    options.initial_depth = 1;
    Array a(options);
    (void)a.set(0, "zero");
    (void)a.set(100, "far");
    (void)a.set(5, "five");
    (void)a.seek(42);

    std::string blob;
    goblin::core::snapshot::Writer w(blob);
    a.save(w);
    goblin::core::snapshot::Reader r(blob);
    Array b = Array::load(r);
    assert(b.implementation() == ArrayImplementation::Classic);
    assert(b.count() == 3);
    assert(b.get(0) == "zero");
    assert(b.get(5) == "five");
    assert(b.get(100) == "far");
    assert(b.next_insert() == 42);
  }
  {
    ArrayOptions options;
    options.implementation = ArrayImplementation::Realtime;
    options.slice_slots = 8;
    options.dir_fanout = 8;
    options.initial_depth = 2;
    Array a(options);
    (void)a.set(0, "rt");
    (void)a.set(63, "edge");  // 8 * 8^2 = 512 capacity; 63 in range
    std::string blob;
    goblin::core::snapshot::Writer w(blob);
    a.save(w);
    goblin::core::snapshot::Reader r(blob);
    Array b = Array::load(r);
    assert(b.is_realtime());
    assert(b.get(0) == "rt");
    assert(b.get(63) == "edge");
    assert(b.depth() == 2);
  }

  // Store GOBLIN.SAVE / load path includes arrays.
  {
    StoreOptions opts;
    opts.array_slice_slots = 32;
    opts.array_initial_depth = 1;
    Store store(opts);
    auto& a = store.get_or_create_array(
        "arr", ArrayImplementation::Classic);
    (void)a.set(1, "one");
    (void)a.set(2, "two");
    auto& rt = store.get_or_create_array("rt", ArrayImplementation::Realtime);
    (void)rt.set(0, "hot");

    std::stringstream ss;
    store.save(ss, /*with_accelerator=*/false);
    assert(ss.good());

    Store loaded(opts);
    const auto stats = loaded.load(ss);
    assert(stats.keys >= 2);
    const auto* la = loaded.find_array("arr");
    assert(la != nullptr);
    assert(la->get(1) == "one");
    assert(la->get(2) == "two");
    const auto* lrt = loaded.find_array("rt");
    assert(lrt != nullptr);
    assert(lrt->is_realtime());
    assert(lrt->get(0) == "hot");
  }

  // RESP command surface: reserve is RT-qualified, executes outside the hot
  // path, reports its bounds, and keeps an emptied reserved key provisioned.
  {
    StoreOptions options;
    options.array_slice_slots = 16;
    options.array_initial_depth = 1;
    Store store(options);
    assert(execute_fields(store, {"GOBLIN.RT.ARRESERVE", "reserved", "31",
                                  "2", "64"}) == ":1\r\n");
    const auto* array = store.find_array("reserved");
    assert(array != nullptr);
    assert(array->is_realtime());
    assert(array->realtime_reserved());
    assert(execute_fields(store, {"GOBLIN.RT.ARSET", "reserved", "0", "a"}) ==
           ":1\r\n");
    assert(execute_fields(store, {"GOBLIN.RT.ARSET", "reserved", "1", "b"}) ==
           ":1\r\n");
    const auto exhausted =
        execute_fields(store, {"GOBLIN.RT.ARSET", "reserved", "2", "c"});
    assert(exhausted.starts_with("-ERR RT array reserved value capacity"));
    assert(execute_fields(store, {"GOBLIN.RT.ARDEL", "reserved", "0", "1"}) ==
           ":2\r\n");
    array = store.find_array("reserved");
    assert(array != nullptr);
    assert(array->count() == 0);
    assert(array->realtime_reserved());
  }

  return 0;
}
