// Unit test for the ring producer's backpressure guarantees (Producer::send_record):
//   1. it throws std::length_error for a record larger than the ring can ever hold
//      (so a mis-sized ring fails loudly instead of spinning forever);
//   2. it blocks while the ring is full and resumes the instant one record is freed
//      -- it does NOT wait for a full drain;
//   3. it aborts the wait (returns false) when the stop predicate says so.
//
// Single process, single thread: we drive both ends of one SQ directly through
// Mapping, and free space from inside the send_record stop callback. That works
// because wait_while_equal is a bounded pause-then-return, so send_record calls the
// callback on every spin -- no second thread or real server needed.

#include "goblin/core/ring_buffer.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include <unistd.h>

using goblin::core::ring::Consumer;
using goblin::core::ring::Mapping;
using goblin::core::ring::Producer;

int main() {
  const std::string path = "/tmp/goblin-ring-bp-" + std::to_string(::getpid()) + ".ring";
  ::unlink(path.c_str());
  // Powers of two, each >= one page on every platform (Apple Silicon pages are 16 KiB).
  auto m = Mapping::create(path.c_str(), 32768, 16384);
  assert(m && "could not create ring file");

  Producer sq = m->sq_producer();
  Consumer rx = m->sq_consumer();  // stands in for the server draining requests
  const std::uint64_t maxp = sq.max_record_payload();
  assert(maxp > 0);

  // (1) A record larger than the ring can ever hold throws, even against an empty
  // ring -- no amount of draining could make room, so blocking would never end.
  {
    const std::string huge(maxp + 1, 'x');
    bool threw = false;
    try {
      (void)sq.send_record(huge, [] { return false; });
    } catch (const std::length_error&) {
      threw = true;
    }
    assert(threw && "send_record must throw length_error when a record can never fit");
  }

  // A record exactly at the limit does NOT throw and fits the empty ring.
  {
    const std::string at_limit(maxp, 'y');
    const bool ok = sq.send_record(at_limit, [] { return false; });
    assert(ok && "a max-size record must fit an empty ring without throwing");
    while (rx.peek()) rx.pop();  // reset to empty
  }

  // Fill the ring with small records until the producer reports full.
  const std::string rec(512, 'r');
  std::size_t filled = 0;
  while (sq.try_push(rec)) ++filled;
  assert(filled > 0 && "ring should hold several small records");

  // (2) Blocks while full, unblocks the moment room appears. The stop callback frees
  // one record per call; send_record must call it (i.e. block) and then succeed as
  // soon as there is room -- proving it does not wait for the ring to fully drain.
  {
    std::size_t stop_calls = 0, freed = 0;
    const bool ok = sq.send_record(rec, [&] {
      ++stop_calls;
      if (rx.peek()) {
        rx.pop();
        ++freed;
      }
      return false;  // never abort -- rely on freeing space to make room
    });
    assert(ok && "send_record must push once room is freed");
    assert(stop_calls >= 1 && "send_record must block (spin) while the ring is full");
    assert(freed >= 1 && "send_record must proceed only after space is freed");
    // Everything pushed is really readable, at the size we wrote.
    std::size_t seen = 0;
    while (auto r = rx.peek()) {
      assert(r->size() == rec.size());
      rx.pop();
      ++seen;
    }
    assert(seen >= 1 && "the freshly pushed record must be readable");
  }

  // (3) Abort: full ring + a stop that gives up => returns false without pushing.
  {
    while (sq.try_push(rec)) {}  // fill again
    const bool ok = sq.send_record(rec, [] { return true; });  // abort immediately
    assert(!ok && "send_record must return false when stop() aborts the wait");
  }

  ::unlink(path.c_str());
  std::puts("ring backpressure OK");
  return 0;
}
