// Unit test for mirror-mapped rings (the double-mapping that lets a record straddle
// the ring end as one contiguous span, so no WRAP fillers are needed).
//
//   1. A default create() mirror-maps where the platform allows it, and a record
//      positioned to cross the physical end still round-trips byte-for-byte -- proof
//      the second mapping folds the overflow onto offset 0.
//   2. create(..., allow_mirror=false) forces the portable single-map + WRAP-filler
//      scheme, and the same seam-crossing traffic round-trips there too.
//
// Single process, single thread: drive both ends of one SQ directly through Mapping,
// pushing then immediately consuming each record so the ring never fills while the
// tail walks several laps across the seam.

#include "goblin/core/ring_buffer.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include <fcntl.h>
#include <unistd.h>

using goblin::core::ring::Consumer;
using goblin::core::ring::Mapping;
using goblin::core::ring::Producer;
namespace ring = goblin::core::ring;

namespace {

// Padded on-ring size of a record: one control line plus the payload rounded up.
std::uint64_t padded(std::size_t n) {
  return ring::kCacheLine + ring::align_up(n, ring::kCacheLine);
}

// A payload whose bytes depend on the lap index, so a mis-aligned or mirror-folded
// read is caught rather than silently matching.
std::string make_payload(std::size_t len, int lap) {
  std::string s(len, '\0');
  for (std::size_t i = 0; i < len; ++i) {
    s[i] = static_cast<char>((i * 7 + lap * 31 + 13) & 0xFF);
  }
  return s;
}

// Push+pop `iters` records one at a time so the tail walks across the ring end
// several times, verifying every record reads back byte-for-byte. Returns how many
// records physically straddled the seam (payload crossing the end).
std::size_t exercise_seam(Mapping& m, int iters) {
  Producer sq = m.sq_producer();
  Consumer rx = m.sq_consumer();
  const std::uint64_t cap = m.sq_capacity();
  const std::uint64_t mask = cap - 1;
  const std::size_t len = 300;  // padded 384; strides the 32 KiB ring unevenly
  std::uint64_t pos = 0;        // our shadow of the absolute tail position
  std::size_t straddles = 0;

  for (int i = 0; i < iters; ++i) {
    const std::uint64_t off = pos & mask;
    if (off + ring::kCacheLine + len > cap) {
      ++straddles;  // this record's payload crosses the physical end
    }
    const std::string payload = make_payload(len, i);
    const bool ok = sq.try_push(payload);
    assert(ok && "try_push should always fit -- we pop every record immediately");
    const auto got = rx.peek();
    assert(got && "a just-pushed record must be visible");
    assert(got->size() == len && "record length preserved");
    assert(*got == payload && "record bytes preserved across the ring seam");
    rx.pop();
    pos += padded(len);
  }
  return straddles;
}

}  // namespace

int main() {
  const std::string base = "/tmp/goblin-ring-mirror-" + std::to_string(::getpid());

  // O_CREAT publishes the pathname before the server can size and initialize the
  // file. A client racing that window must retry rather than mmap past EOF and
  // take SIGBUS while inspecting the header.
  {
    const std::string path = base + "-empty.ring";
    ::unlink(path.c_str());
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(::close(fd) == 0);
    assert(!Mapping::open(path.c_str()) &&
           "an unsized ring file must not be mapped");
    ::unlink(path.c_str());
  }

  // (1) Default create: mirror where supported. Exercise seam-crossing traffic.
  {
    const std::string path = base + "-a.ring";
    ::unlink(path.c_str());
    auto m = Mapping::create(path.c_str(), 32768, 16384);  // allow_mirror defaults true
    assert(m && "could not create ring");
    const std::size_t straddles = exercise_seam(*m, 512);
    assert(straddles > 0 && "the traffic pattern must cross the seam");
    if (m->mirror()) {
      std::printf("mirror mode active; %zu records straddled the seam and round-tripped\n",
                  straddles);
    } else {
      std::printf("mirror unavailable on this platform; wrap fallback round-tripped\n");
    }
    ::unlink(path.c_str());
  }

  // (2) Forced single-map + WRAP fillers: the same traffic must round-trip too.
  {
    const std::string path = base + "-b.ring";
    ::unlink(path.c_str());
    auto m = Mapping::create(path.c_str(), 32768, 16384, /*allow_mirror=*/false);
    assert(m && "could not create wrap-mode ring");
    assert(!m->mirror() && "allow_mirror=false must yield a single-mapped ring");
    const std::size_t straddles = exercise_seam(*m, 512);
    assert(straddles > 0 && "the traffic pattern must reach the seam");
    std::printf("wrap mode round-tripped seam-crossing traffic via fillers\n");
    ::unlink(path.c_str());
  }

  std::puts("ring mirror OK");
  return 0;
}
