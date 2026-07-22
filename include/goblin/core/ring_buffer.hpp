#pragma once

// Shared-memory ring buffers for the lowest-latency path into the server.
//
// Each `--ring <path> <size>` maps one file that carries an io_uring-style pair
// of single-producer/single-consumer rings:
//
//   * SQ (submission queue): client -> server. The client writes RESP command
//     bytes; the server drains them. This is the request direction.
//   * CQ (completion queue): server -> client. The server writes RESP reply
//     bytes; the client drains them. This is the response direction.
//
// Layout of the file (all offsets page-aligned so the data regions can be mmap'd
// directly):
//
//   [ Header (one page) ]   magic/version/capacities + the four ring indices,
//                           each index on its own cache line so the producer and
//                           consumer of a direction never share a line.
//   [ SQ data (capacity) ]  power-of-two byte ring
//   [ CQ data (capacity) ]  power-of-two byte ring
//
// Records: each record begins on a 64-byte cache-line boundary. The first cache
// line is a control line holding the payload length and a flag; the RESP payload
// starts on the *next* cache line, so the payload itself is cache-aligned (a line
// of redis messages "starts on a cache boundary, but can be longer than a cache
// line"). The record is padded up to the next cache line. The producer publishes a
// record by advancing its tail with a release store; the consumer reads up to the
// tail with an acquire load, so the byte writes are visible before the index move.
//
// A data record never straddles the physical end of the ring: when one would not
// fit before the end, the producer writes a one-line WRAP filler that tells the
// consumer to skip to offset 0, then places the record at the start. So every
// payload is a single contiguous span and `offset = position & (capacity - 1)` is
// the only index math -- with no reliance on double-mapping tricks that not every
// OS supports.
//
// The indices are monotonically increasing 64-bit byte counters (they never wrap
// in any realistic runtime); free space is `capacity - (tail - head)` and the
// ring is empty when `head == tail`. Positions are reduced into the buffer with a
// mask (capacity is a power of two).

#include "goblin/core/numa.hpp"

#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
// hugetlb support: fstatfs to detect a hugetlbfs mount, and dirent/cstdio to
// enumerate the available huge-page sizes and mounts. All Linux-only -- hugetlb
// does not exist on macOS, where the whole feature compiles out.
#include <dirent.h>
#include <sys/vfs.h>
#include <cstdio>
#include <vector>
#ifndef HUGETLBFS_MAGIC
#define HUGETLBFS_MAGIC 0x958458f6
#endif
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace goblin::core::ring {

// Record control/payload alignment. 64 bytes is enough for record false-sharing
// avoidance on every ISA we care about (x86 line = 64; Apple's 128-byte L2
// prefetch does not require 128-byte *record* padding and costs bandwidth on
// the tiny PING/ZADD-1 path).
inline constexpr std::size_t kCacheLine = 64;
// Index-word isolation. Apple Silicon L1/L2 lines are 128 bytes and the core
// prefetches 128-byte pairs, so two 64-byte-adjacent indices still bounce.
// Keep the four ring index words 128-byte-strided on Apple; 64 elsewhere.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kIndexLine = 128;
#else
inline constexpr std::size_t kIndexLine = 64;
#endif
inline constexpr std::uint32_t kMagic = 0x474E5247;  // 'GRNG'
inline constexpr std::uint32_t kVersion = 3;  // v3 identifies the server process

// Pure-spin budget before parking on a ring index (macOS). Must cover a healthy
// sub-µs RTT with headroom for brief peer preemption -- parking too early turns
// every round trip into a ~10–50 µs scheduler hop. ~1M iters is multi-ms of spin
// at empty-poll cost; only a truly stalled peer (or idle server) parks.
inline constexpr unsigned kAdaptiveSpinIters = 1u << 20;
// Park timeout once we give up spinning. Bounds the tail when a peer is parked
// or descheduled; stop()/deadline checks still fire because wait returns.
inline constexpr std::uint64_t kAdaptiveWaitNs = 100'000;  // 100 µs

// The system page size. mmap file offsets must be a multiple of it, so the header
// region and every ring capacity are aligned to it. It is 4 KiB on x86, but 16 KiB
// on Apple Silicon and configurable on other ARM/LoongArch systems, so it must be
// read at runtime rather than assumed.
[[nodiscard]] inline std::uint64_t page_size() noexcept {
  static const std::uint64_t ps = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  return ps;
}

// The header occupies one page; the SQ and CQ data regions follow it. (On a
// hugetlbfs-backed ring the granule is the huge-page size instead -- see Mapping.)
[[nodiscard]] inline std::uint64_t header_bytes() noexcept { return page_size(); }

#if defined(__linux__)
namespace detail {

// The smallest huge-page size the kernel offers that is strictly larger than the
// base page, in bytes, or 0 if none. Enumerates /sys/kernel/mm/hugepages/hugepages-*kB
// (each present directory is a supported size), so it returns 2 MiB on 4 KiB-page x86
// and the 32 MiB-class size on a 16 KiB-page box without hardcoding.
[[nodiscard]] inline std::uint64_t smallest_hugepage_over_base() noexcept {
  DIR* d = ::opendir("/sys/kernel/mm/hugepages");
  if (d == nullptr) return 0;
  const std::uint64_t base = page_size();
  std::uint64_t best = 0;
  for (const dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    unsigned long long kib = 0;
    if (std::sscanf(e->d_name, "hugepages-%llukB", &kib) != 1) continue;
    const std::uint64_t bytes = static_cast<std::uint64_t>(kib) * 1024;
    if (bytes > base && (best == 0 || bytes < best)) best = bytes;
  }
  ::closedir(d);
  return best;
}

// The mount point of a hugetlbfs filesystem whose page size equals `hugepage_bytes`,
// or empty if none is mounted. Confirms the size with statfs(f_bsize) rather than
// trusting the pagesize= mount-option string.
[[nodiscard]] inline std::string find_hugetlbfs_mount(std::uint64_t hugepage_bytes) {
  std::FILE* f = std::fopen("/proc/mounts", "re");
  if (f == nullptr) return {};
  std::string result;
  char line[4096];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    char dev[1024], dir[1024], type[256];
    if (std::sscanf(line, "%1023s %1023s %255s", dev, dir, type) != 3) continue;
    if (std::strcmp(type, "hugetlbfs") != 0) continue;
    struct statfs sfs;
    if (::statfs(dir, &sfs) == 0 &&
        static_cast<std::uint64_t>(sfs.f_bsize) == hugepage_bytes) {
      result = dir;
      break;
    }
  }
  std::fclose(f);
  return result;
}

}  // namespace detail
#endif  // __linux__

// Spin-loop relax: hint the core to back off so a sibling hyperthread (and the
// remote core writing our pages) makes progress, and so a busy-poll does not burn
// the pipeline on failed speculation. The atomics carry the ordering; this is a
// throughput/power hint only.
//
// Apple Silicon: do NOT use `yield` here. On Darwin a pure yield-spin is treated as
// low-value work and the scheduler parks the thread for ~10–15 µs (the macOS ring
// p99 cliff). A compiler barrier alone re-reads the atomic without volunteering
// the core. x86 keeps PAUSE; other ARM keeps YIELD.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm__))
  __asm__ __volatile__("" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#elif defined(__loongarch__)
  // LoongArch has no dedicated pause; the kernel's cpu_relax() is a compiler
  // barrier, which is enough here since the acquire load already re-reads memory.
  __asm__ __volatile__("dbar 0" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

// macOS: request USER_INTERACTIVE QoS so the scheduler keeps this busy-poll on a
// performance core at high priority. Apple Silicon has no core pinning; QoS is the
// idiomatic lever -- unlike THREAD_TIME_CONSTRAINT (which throttles a spin loop) or two
// SCHED_FIFO spinners (which contend for the few P-cores). Best-effort; a no-op off macOS.
// Call on BOTH the server and the client threads that spin on a ring.
inline void set_busy_poll_thread_realtime() noexcept {
#if defined(__APPLE__)
  // Relative priority -15 = highest within USER_INTERACTIVE (range -15..0).
  (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, -15);
#endif
}

// Wait while a ring index still equals `expected`. The hot path is pure spin
// (sub-µs RTT). On macOS, after a long empty budget we briefly park with a
// timeout so a descheduled peer does not pin a P-core forever.
//
// Intentionally timeout-based (no publish-side wake): a wake syscall on every
// try_push/pop costs ~µs even with zero waiters and destroys the sub-µs path.
// The timeout re-polls; a live peer almost never reaches this phase.
//
// `word` is the shared mmap address of a u64 index.
inline void wait_while_equal(std::uint64_t* word, std::uint64_t expected,
                             unsigned& spin) noexcept {
  if (++spin < kAdaptiveSpinIters) {
    cpu_relax();
    return;
  }
  spin = 0;
#if defined(__APPLE__)
  // SHARED: client and server map the ring at different VAs; the kernel keys
  // by physical page + offset.
  (void)::os_sync_wait_on_address_with_timeout(
      word, expected, sizeof(*word), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
      OS_CLOCK_MACH_ABSOLUTE_TIME, kAdaptiveWaitNs);
#else
  (void)word;
  (void)expected;
  cpu_relax();
#endif
}

// Parse a size with an optional binary suffix: "4096" (bytes), "4kb"/"4k" (KiB),
// "1mb"/"1m" (MiB), "2gb"/"2g" (GiB), "512b" (bytes). Case-insensitive, base 1024.
[[nodiscard]] inline std::optional<std::uint64_t> parse_size(std::string_view text) {
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  if (digits == 0) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + digits, value);
  if (ec != std::errc{} || ptr != text.data() + digits) {
    return std::nullopt;
  }
  std::string_view suffix = text.substr(digits);
  const auto ieq = [suffix](std::string_view s) {
    if (suffix.size() != s.size()) {
      return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
      const char a = suffix[i] >= 'A' && suffix[i] <= 'Z' ? suffix[i] + 32 : suffix[i];
      if (a != s[i]) {
        return false;
      }
    }
    return true;
  };
  std::uint64_t mult = 1;
  if (suffix.empty() || ieq("b")) {
    mult = 1;
  } else if (ieq("k") || ieq("kb")) {
    mult = 1024ULL;
  } else if (ieq("m") || ieq("mb")) {
    mult = 1024ULL * 1024;
  } else if (ieq("g") || ieq("gb")) {
    mult = 1024ULL * 1024 * 1024;
  } else {
    return std::nullopt;
  }
  if (value != 0 && value > (~std::uint64_t{0}) / mult) {
    return std::nullopt;  // overflow
  }
  return value * mult;
}

// Round a requested size up to a power-of-two ring capacity of at least one page.
// A power of two >= the (power-of-two) page size is a multiple of it, so the data
// regions stay page-aligned for mmap.
[[nodiscard]] inline std::uint64_t capacity_for(std::uint64_t bytes) noexcept {
  const std::uint64_t floor = page_size();
  return std::bit_ceil(bytes < floor ? floor : bytes);
}

[[nodiscard]] inline std::uint64_t align_up(std::uint64_t x, std::uint64_t a) noexcept {
  return (x + (a - 1)) & ~(a - 1);
}

// The on-file header, overlaid on the mapped page. The index fields are plain
// integers (zero-filled by ftruncate) accessed through std::atomic_ref, so there
// is no cross-process atomic-construction question -- the storage is just bytes.
struct alignas(kIndexLine) Header {
  std::uint32_t magic;       // written last on create; readers gate on it
  std::uint32_t version;
  std::uint64_t sq_capacity;  // power of two
  std::uint64_t cq_capacity;  // power of two
  std::uint64_t sq_offset;    // file offset of SQ data
  std::uint64_t cq_offset;    // file offset of CQ data
  // Reconnect handshake. A client bumps `epoch` on (re)connect; the server, seeing
  // epoch != epoch_ack, drains both queues (discarding whatever a dead predecessor
  // left in flight), re-arms protocol detection, and echoes the value into
  // `epoch_ack` so the client knows it may proceed. This recovers a ring a client
  // abandoned mid-crash without restarting the server. Client owns `epoch`, server
  // owns `epoch_ack`; both read-mostly, so sharing the config line is fine.
  std::uint64_t epoch;
  std::uint64_t epoch_ack;
  // 1 if the SQ/CQ data regions are mirror-mapped (each mapped twice at adjacent
  // virtual addresses) so a record may straddle the ring end as one contiguous span
  // and no WRAP fillers are written; 0 for the portable single-map + WRAP-filler
  // scheme. create() sets it (falling back to 0 when the double-map is unavailable);
  // open() reads it, so the producer and consumer always agree on whether fillers
  // exist. Read-mostly config, so it shares the config line with the fields above.
  std::uint32_t mirror;
  // Server process that owns this mapping. Replica-ring followers use it as a
  // liveness check; it also distinguishes a recreated mapping whose normal-file
  // inode did not change. Immutable after magic is published.
  std::uint32_t owner_pid;
  // Each index on its own kIndexLine: the producer writes one, the consumer the
  // other, and they must not share a line (or an Apple 128-byte prefetch pair)
  // or every publish bounces the cache.
  alignas(kIndexLine) std::uint64_t sq_head;  // consumer (server) owns
  alignas(kIndexLine) std::uint64_t sq_tail;  // producer (client) owns
  alignas(kIndexLine) std::uint64_t cq_head;  // consumer (client) owns
  alignas(kIndexLine) std::uint64_t cq_tail;  // producer (server) owns
};
static_assert(sizeof(Header) == 5 * kIndexLine);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
// sizeof(Header) fits in one page on every supported system (smallest page 4 KiB).

// Record control-line flags (stored in the second word of the control line).
inline constexpr std::uint32_t kFlagData = 0;  // a payload-bearing record
inline constexpr std::uint32_t kFlagWrap = 1;  // filler: skip to offset 0

namespace detail {

inline void put_u32(char* p, std::uint32_t v) noexcept {
  std::memcpy(p, &v, sizeof(v));
}
[[nodiscard]] inline std::uint32_t get_u32(const char* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

}  // namespace detail

// A single-consumer view over one direction. Reads records the producer has
// published; skips WRAP fillers so every payload it returns is contiguous.
//
// We deliberately do *not* cache the producer's tail: reconnect's drain can
// rewind cq_tail (cq_tail := cq_head), so a stale high snapshot would invent
// phantom records. The win here is cheaper: after advance_to_record succeeds,
// head_pos_ holds the data record's absolute position so peek/pop share one
// head load instead of three.
class Consumer {
 public:
  Consumer() = default;
  Consumer(std::uint64_t* head, std::uint64_t* tail, char* data,
           std::uint64_t capacity, bool mirror = false) noexcept
      : head_(head), tail_(tail), data_(data), capacity_(capacity),
        mask_(capacity - 1), mirror_(mirror),
        // We own head: seed the local cursor so empty-spin peeks never re-load it.
        head_pos_(std::atomic_ref<std::uint64_t>(*head).load(
            std::memory_order_relaxed)) {}

  // Is there a published data record to read?
  [[nodiscard]] bool has_record() noexcept { return advance_to_record(); }

  // The next record's payload, as a contiguous view into the ring, or nullopt
  // when empty. Valid until pop().
  [[nodiscard]] std::optional<std::string_view> peek() noexcept {
    if (!advance_to_record()) {
      return std::nullopt;
    }
    const std::uint64_t h = head_pos_;
    const char* base = data_ + (h & mask_);
    const std::uint32_t len = detail::get_u32(base);
    return std::string_view(base + kCacheLine, len);
  }

  // Discard the data record most recently peek()'d, freeing its space.
  void pop() noexcept {
    const std::uint64_t h = head_pos_;
    const std::uint32_t len = detail::get_u32(data_ + (h & mask_));
    const std::uint64_t next = h + kCacheLine + align_up(len, kCacheLine);
    store_head(next);
    head_pos_ = next;
  }

  // Adaptive wait until a published record appears (or the caller should recheck
  // a stop/deadline). Spins briefly, then parks on the tail word (macOS).
  void wait_for_record() noexcept {
    const std::uint64_t h = head_pos_;
    const std::uint64_t t = std::atomic_ref<std::uint64_t>(*tail_).load(
        std::memory_order_acquire);
    if (h != t) {
      empty_spins_ = 0;
      return;
    }
    wait_while_equal(tail_, t, empty_spins_);
  }

 private:
  void store_head(std::uint64_t v) const noexcept {
    std::atomic_ref<std::uint64_t>(*head_).store(v, std::memory_order_release);
  }
  // Advance past any WRAP fillers; true once a data record sits at head.
  // Uses head_pos_ as the sole head cursor (we are the single consumer) so the
  // empty-spin path is just an acquire load of the remote tail.
  [[nodiscard]] bool advance_to_record() noexcept {
    std::uint64_t h = head_pos_;
    for (;;) {
      const std::uint64_t t = std::atomic_ref<std::uint64_t>(*tail_).load(
          std::memory_order_acquire);
      if (h == t) {
        return false;
      }
      empty_spins_ = 0;  // saw work; reset adaptive park counter
      // Mirror map: the producer never writes fillers (a record straddles the end as
      // one contiguous span), so head already sits on a data record.
      if (mirror_) {
        head_pos_ = h;
        return true;
      }
      const std::uint64_t off = h & mask_;
      if (detail::get_u32(data_ + off + sizeof(std::uint32_t)) == kFlagWrap) {
        h += capacity_ - off;  // jump to the ring boundary (offset 0)
        store_head(h);
        head_pos_ = h;
        continue;
      }
      head_pos_ = h;
      return true;
    }
  }

  std::uint64_t* head_ = nullptr;
  std::uint64_t* tail_ = nullptr;
  char* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t mask_ = 0;
  bool mirror_ = false;
  std::uint64_t head_pos_ = 0;
  unsigned empty_spins_ = 0;
};

// A single-producer view over one direction. Writes payloads as cache-aligned
// records, splitting anything larger than a single record across several.
//
// SPSC optimization: `cached_head_` is a local snapshot of the consumer's head,
// refreshed only when free space looks tight. A 1 MiB ring almost never hits
// that path on the latency bench, so the hot try_push avoids an acquire load of
// the remote head line on every publish (twice per round trip).
class Producer {
 public:
  Producer() = default;
  Producer(std::uint64_t* head, std::uint64_t* tail, char* data,
           std::uint64_t capacity, bool mirror = false) noexcept
      : head_(head), tail_(tail), data_(data), capacity_(capacity),
        mask_(capacity - 1), mirror_(mirror),
        // A record uses one control line plus the padded payload; cap a single
        // record at half the ring so at least two can coexist (pipelining) and no
        // single record needs a fully-drained ring.
        max_payload_(capacity / 2 - kCacheLine),
        // Seed from live indices so a reconnect-drain (which moves head/tail
        // under us) is recovered on the next free-space miss rather than left
        // with a stale zero cache forever.
        cached_head_(std::atomic_ref<std::uint64_t>(*head).load(
            std::memory_order_relaxed)),
        tail_pos_(std::atomic_ref<std::uint64_t>(*tail).load(
            std::memory_order_relaxed)) {}

  [[nodiscard]] std::uint64_t max_record_payload() const noexcept {
    return max_payload_;
  }

  // Try to enqueue one record of `payload` (<= max_record_payload). Returns false
  // without writing when there is not enough free space. If the record would
  // straddle the ring's end, a WRAP filler is written first and the record placed
  // at offset 0 -- both published by the single tail store, so the consumer sees
  // the filler and the record together.
  [[nodiscard]] bool try_push(std::string_view payload) noexcept {
    const std::uint64_t padded = kCacheLine + align_up(payload.size(), kCacheLine);
    std::uint64_t t = tail_pos_;
    if (mirror_) {
      // Mirror map: the region is mapped twice back to back, so a record can straddle
      // the physical end and be written as one contiguous span (the second mapping
      // folds the overflow onto offset 0). No filler is ever needed, so a record needs
      // exactly `padded` free bytes -- and the ring wastes no tail bytes at the seam.
      if (capacity_ - (t - cached_head_) < padded) {
        cached_head_ = std::atomic_ref<std::uint64_t>(*head_).load(
            std::memory_order_acquire);
        if (capacity_ - (t - cached_head_) < padded) {
          return false;
        }
      }
      char* base = data_ + (t & mask_);
      const std::uint64_t ctrl = static_cast<std::uint32_t>(payload.size()) |
                                 (static_cast<std::uint64_t>(kFlagData) << 32);
      std::memcpy(base, &ctrl, sizeof(ctrl));
      std::memcpy(base + kCacheLine, payload.data(), payload.size());
      const std::uint64_t next = t + padded;
      std::atomic_ref<std::uint64_t>(*tail_).store(next, std::memory_order_release);
      tail_pos_ = next;
      return true;
    }
    const std::uint64_t off = t & mask_;
    const std::uint64_t to_end = capacity_ - off;
    const std::uint64_t need = padded > to_end ? to_end + padded : padded;
    if (capacity_ - (t - cached_head_) < need) {
      // Looks full against the local head snapshot: re-acquire the consumer's
      // head (pairs with its release store of free space) and recheck.
      cached_head_ = std::atomic_ref<std::uint64_t>(*head_).load(
          std::memory_order_acquire);
      if (capacity_ - (t - cached_head_) < need) {
        return false;
      }
    }
    if (padded > to_end) {
      // Record will not fit before the end: drop a WRAP filler that fills to the
      // boundary, then continue at offset 0.
      char* filler = data_ + off;
      // One 8-byte store: length 0 | flag WRAP in the low/high 32-bit LE words.
      const std::uint64_t wrap_ctrl =
          (static_cast<std::uint64_t>(kFlagWrap) << 32);
      std::memcpy(filler, &wrap_ctrl, sizeof(wrap_ctrl));
      t += to_end;  // now (t & mask_) == 0
    }
    char* base = data_ + (t & mask_);
    // One 8-byte store: payload length | flag DATA (0). Avoids two put_u32s.
    const std::uint64_t ctrl = static_cast<std::uint32_t>(payload.size()) |
                               (static_cast<std::uint64_t>(kFlagData) << 32);
    std::memcpy(base, &ctrl, sizeof(ctrl));
    std::memcpy(base + kCacheLine, payload.data(), payload.size());
    const std::uint64_t next = t + padded;
    std::atomic_ref<std::uint64_t>(*tail_).store(next, std::memory_order_release);
    tail_pos_ = next;
    // Optional wake for a parked consumer (macOS). Only fires after the peer has
    // exhausted its pure-spin budget; when nobody is waiting the kernel returns
    // ENOENT. Still a syscall, so we only wake if the wait side has been used
    // heavily -- see wait_while_equal. Unconditional wake costs ~µs/op and kills
    // the sub-µs path, so the default wait is timeout-based (no wake required).
    return true;
  }

  // Enqueue all of `payload`, splitting into records and spinning (cpu_relax) for
  // space between them. `stop()` aborts the wait (returns early) -- e.g. on
  // shutdown or a client-side timeout.
  template <class StopFn>
  void send(std::string_view payload, StopFn&& stop) noexcept {
    // Hot path: one record (every PING / small ZADD). Avoid the chunking loop.
    if (payload.size() <= max_payload_) {
      unsigned spins = 0;
      while (!try_push(payload)) {
        // Full ring: adaptive wait on the consumer's head advancing.
        const std::uint64_t h = std::atomic_ref<std::uint64_t>(*head_).load(
            std::memory_order_acquire);
        wait_while_equal(head_, h, spins);
        if (stop()) {
          return;
        }
      }
      return;
    }
    while (!payload.empty()) {
      const std::size_t chunk =
          payload.size() < max_payload_ ? payload.size() : max_payload_;
      const std::string_view piece = payload.substr(0, chunk);
      unsigned spins = 0;
      while (!try_push(piece)) {
        const std::uint64_t h = std::atomic_ref<std::uint64_t>(*head_).load(
            std::memory_order_acquire);
        wait_while_equal(head_, h, spins);
        if (stop()) {
          return;
        }
      }
      payload.remove_prefix(chunk);
    }
  }

  // Enqueue `payload` as a SINGLE ring record -- never split. Spins (cpu_relax) for
  // space until the whole record fits or `stop()` returns true; returns true when
  // pushed, false if `stop()` aborted the wait. Unlike send(), which chunks a large
  // payload into record-sized pieces (a byte stream the reader reassembles), this
  // keeps one message atomic on the ring -- the reader never sees a partial record.
  //
  // The wait is released the moment there is room for THIS record, not when the ring
  // is empty: try_push succeeds as soon as free space >= the record's padded size.
  //
  // Throws std::length_error if `payload` is larger than max_record_payload(): a
  // record that big can never fit, even in a fully drained ring, so blocking would
  // spin forever. Enlarge the ring (a bigger `--ring <path> <size>`) to send it.
  template <class StopFn>
  bool send_record(std::string_view payload, StopFn&& stop) {
    if (payload.size() > max_payload_) {
      throw std::length_error(
          "ring record (" + std::to_string(payload.size()) +
          " bytes) exceeds the ring's maximum record size (" +
          std::to_string(max_payload_) + " bytes); enlarge the ring");
    }
    unsigned spins = 0;
    while (!try_push(payload)) {
      // Full ring: adaptive wait on the consumer's head advancing, then recheck --
      // the recheck (try_push) succeeds as soon as this record fits.
      const std::uint64_t h = std::atomic_ref<std::uint64_t>(*head_).load(
          std::memory_order_acquire);
      wait_while_equal(head_, h, spins);
      if (stop()) {
        return false;
      }
    }
    return true;
  }

 private:
  std::uint64_t* head_ = nullptr;
  std::uint64_t* tail_ = nullptr;
  char* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t mask_ = 0;
  bool mirror_ = false;
  std::uint64_t max_payload_ = 0;
  std::uint64_t cached_head_ = 0;
  std::uint64_t tail_pos_ = 0;
};

// Owns the mmap'd regions of one ring file. Move-only; unmaps on destruction.
class Mapping {
 public:
  Mapping() = default;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept { move_from(other); }
  Mapping& operator=(Mapping&& other) noexcept {
    if (this != &other) {
      reset();
      move_from(other);
    }
    return *this;
  }
  ~Mapping() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }
  [[nodiscard]] Header* header() const noexcept { return header_; }
  // The backing granule: base page for a normal ring, huge-page size for a
  // hugetlb-backed one. is_hugetlb() is true only for the latter.
  [[nodiscard]] std::uint64_t granule() const noexcept { return granule_; }
  [[nodiscard]] bool is_hugetlb() const noexcept { return granule_ > page_size(); }
  [[nodiscard]] std::uint64_t sq_capacity() const noexcept { return sq_cap_; }
  [[nodiscard]] std::uint64_t cq_capacity() const noexcept { return cq_cap_; }

  // Server side: consume the SQ (requests), produce into the CQ (replies).
  [[nodiscard]] Consumer sq_consumer() const noexcept {
    return Consumer(&header_->sq_head, &header_->sq_tail, sq_, sq_cap_, mirror_);
  }
  [[nodiscard]] Producer cq_producer() const noexcept {
    return Producer(&header_->cq_head, &header_->cq_tail, cq_, cq_cap_, mirror_);
  }
  // Client side: produce into the SQ (requests), consume the CQ (replies).
  [[nodiscard]] Producer sq_producer() const noexcept {
    return Producer(&header_->sq_head, &header_->sq_tail, sq_, sq_cap_, mirror_);
  }
  [[nodiscard]] Consumer cq_consumer() const noexcept {
    return Consumer(&header_->cq_head, &header_->cq_tail, cq_, cq_cap_, mirror_);
  }

  // Whether the data regions are mirror-mapped (records may straddle the ring end).
  [[nodiscard]] bool mirror() const noexcept { return mirror_; }

  // Verify every page of the header + SQ + CQ regions is resident on NUMA `node`.
  // Called after creating the ring under a node-bound policy: a remote ring wrecks
  // the latency it exists for, so the server treats a false here as fatal.
  [[nodiscard]] bool numa_all_local(int node) const noexcept {
    return numa::all_on_node(header_, granule_, node) &&
           numa::all_on_node(sq_, mirror_ ? 2 * sq_cap_ : sq_cap_, node) &&
           numa::all_on_node(cq_, mirror_ ? 2 * cq_cap_ : cq_cap_, node);
  }

  // ---- reconnect handshake (see Header::epoch) -------------------------------
  // Client: request a fresh connection -- bump the epoch and return the new value.
  // The caller then spins on reconnect_acked() before using the ring, so the server
  // has drained any dead predecessor's leftovers first.
  [[nodiscard]] std::uint64_t request_reconnect() const noexcept {
    auto ref = std::atomic_ref<std::uint64_t>(header_->epoch);
    const std::uint64_t next = ref.load(std::memory_order_relaxed) + 1;
    ref.store(next, std::memory_order_release);
    return next;
  }
  // Client: has the server drained and acked this epoch? The acquire pairs with the
  // server's release ack, so its drain stores are visible once this returns true.
  [[nodiscard]] bool reconnect_acked(std::uint64_t epoch) const noexcept {
    return std::atomic_ref<std::uint64_t>(header_->epoch_ack).load(
               std::memory_order_acquire) == epoch;
  }
  // Server: the epoch a client is requesting, and the one we last acked.
  [[nodiscard]] std::uint64_t requested_epoch() const noexcept {
    return std::atomic_ref<std::uint64_t>(header_->epoch).load(
        std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t acked_epoch() const noexcept {
    return std::atomic_ref<std::uint64_t>(header_->epoch_ack).load(
        std::memory_order_relaxed);
  }
  // Server: publish the ack (release, so the client sees the drain the instant it
  // observes the ack).
  void ack_epoch(std::uint64_t epoch) const noexcept {
    std::atomic_ref<std::uint64_t>(header_->epoch_ack)
        .store(epoch, std::memory_order_release);
  }
  // Server: discard everything a dead client left -- any unconsumed request
  // (sq_head := sq_tail) and any unread reply (cq_tail := cq_head). Safe because the
  // handshake keeps the new client quiescent (spinning on the ack) and the old one is
  // gone, so sq_tail and cq_head are stable; the server owns sq_head and cq_tail.
  void drain_for_reconnect() const noexcept {
    const std::uint64_t sqt = std::atomic_ref<std::uint64_t>(header_->sq_tail).load(
        std::memory_order_acquire);
    std::atomic_ref<std::uint64_t>(header_->sq_head)
        .store(sqt, std::memory_order_relaxed);
    const std::uint64_t cqh = std::atomic_ref<std::uint64_t>(header_->cq_head).load(
        std::memory_order_acquire);
    std::atomic_ref<std::uint64_t>(header_->cq_tail)
        .store(cqh, std::memory_order_relaxed);
  }

  // Create (or re-create) a ring file and initialize it. The server calls this.
  // `sq_cap`/`cq_cap` must be powers of two >= one page. When `allow_mirror` is set
  // (the default) it maps each data region twice back to back so records can straddle
  // the ring end contiguously; if that double-map is unavailable it falls back to the
  // single-map + WRAP-filler scheme. The chosen mode is recorded in the header so
  // clients open() it the same way.
  [[nodiscard]] static std::optional<Mapping> create(const char* path,
                                                     std::uint64_t sq_cap,
                                                     std::uint64_t cq_cap,
                                                     bool allow_mirror = true) noexcept {
    const int fd = ::open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
      return std::nullopt;
    }
    // The granule is the base page normally, or the huge-page size when `path` is on
    // hugetlbfs -- in which case sq_cap/cq_cap must already be huge-page multiples
    // (create_hugetlb rounds them up). It sets the header size and data alignment.
    const std::uint64_t granule = detect_granule(fd);
    const std::uint64_t sq_off = granule;
    const std::uint64_t cq_off = granule + sq_cap;
    const std::uint64_t total = granule + sq_cap + cq_cap;
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    Mapping m;
    // Prefer the mirror map; fall back to the single map + WRAP fillers if it fails.
    bool ok = allow_mirror &&
              m.map_all(fd, sq_cap, cq_cap, sq_off, cq_off, granule, /*mirror=*/true);
    if (!ok) {
      ok = m.map_all(fd, sq_cap, cq_cap, sq_off, cq_off, granule, /*mirror=*/false);
    }
    if (!ok) {
      ::close(fd);
      return std::nullopt;
    }
    // Prefault + lock before publishing the magic below: no client can have mapped
    // the ring yet, so it is safe to write-touch (and thus allocate backing for)
    // every data page here on the server that owns the fresh file.
    m.prefault_and_lock(/*write_alloc=*/true);
    Header* h = m.header_;
    h->version = kVersion;
    h->sq_capacity = sq_cap;
    h->cq_capacity = cq_cap;
    h->sq_offset = sq_off;
    h->cq_offset = cq_off;
    h->epoch = 0;
    h->epoch_ack = 0;
    h->mirror = m.mirror_ ? 1u : 0u;
    std::atomic_ref<std::uint32_t>(h->owner_pid)
        .store(static_cast<std::uint32_t>(::getpid()),
               std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->sq_head).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->sq_tail).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->cq_head).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->cq_tail).store(0, std::memory_order_relaxed);
    // Publish magic last so a reader that gates on it sees a fully-init header.
    std::atomic_ref<std::uint32_t>(h->magic).store(kMagic, std::memory_order_release);
    ::close(fd);
    return m;
  }

#if defined(__linux__)
  // Create a hugetlb-backed ring (Linux only). Discovers the smallest huge page
  // larger than the base page and a hugetlbfs mount for it, rounds `requested` up to a
  // power-of-two >= that huge page (so 1 MiB with a 2 MiB huge page becomes 2 MiB),
  // creates the real ring file on the mount, and symlinks `user_path` to it so every
  // client's ordinary open-by-path follows the symlink into hugetlbfs. Returns nullopt
  // (leaving nothing behind) if no huge pages are configured or the mapping fails.
  // The returned Mapping owns both files and unlinks them when it is destroyed.
  [[nodiscard]] static std::optional<Mapping> create_hugetlb(
      const char* user_path, std::uint64_t requested, bool allow_mirror = true) {
    const std::uint64_t huge = detail::smallest_hugepage_over_base();
    if (huge == 0) {
      return std::nullopt;  // no huge-page sizes larger than the base page
    }
    const std::string mount = detail::find_hugetlbfs_mount(huge);
    if (mount.empty()) {
      return std::nullopt;  // no hugetlbfs mount of that size
    }
    std::uint64_t cap = huge;  // huge is a power of two; keep doubling to cover request
    while (cap < requested) {
      cap <<= 1;
    }
    std::string real = mount + "/goblin-ring-" + std::to_string(::getpid()) + "-" +
                       base_name(user_path);
    ::unlink(real.c_str());
    auto m = create(real.c_str(), cap, cap, allow_mirror);
    if (!m) {
      ::unlink(real.c_str());
      return std::nullopt;
    }
    // Publish the user-facing path as a symlink into hugetlbfs.
    ::unlink(user_path);
    if (::symlink(real.c_str(), user_path) != 0) {
      ::unlink(real.c_str());  // m's destructor unmaps; nothing is symlinked yet
      return std::nullopt;
    }
    m->real_path_ = std::move(real);
    m->link_path_ = user_path;
    return m;
  }
#endif  // __linux__

  // Open an existing, initialized ring file. The client calls this. Returns
  // nullopt if the file is missing, not yet initialized (magic unset), or the
  // version mismatches.
  [[nodiscard]] static std::optional<Mapping> open(const char* path) noexcept {
    const int fd = ::open(path, O_RDWR);
    if (fd < 0) {
      return std::nullopt;
    }
    // Detect the granule from the fd (huge-page size on hugetlbfs, else base page):
    // the client that follows a symlink into hugetlbfs must map the header at the
    // huge-page granularity too, since hugetlbfs refuses a sub-huge-page mmap length.
    const std::uint64_t granule = detect_granule(fd);
    struct stat file_stat {};
    // O_CREAT makes the path visible before the server's ftruncate(). Mapping a
    // page from that transient zero-length file succeeds, but touching it raises
    // SIGBUS. Treat an incomplete backing file like an unpublished header and let
    // the client's existing open loop retry.
    if (::fstat(fd, &file_stat) != 0 || file_stat.st_size < 0 ||
        static_cast<std::uint64_t>(file_stat.st_size) < granule) {
      ::close(fd);
      return std::nullopt;
    }
    const auto file_size = static_cast<std::uint64_t>(file_stat.st_size);
    void* hp = ::mmap(nullptr, granule, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hp == MAP_FAILED) {
      ::close(fd);
      return std::nullopt;
    }
    Header* h = static_cast<Header*>(hp);
    if (std::atomic_ref<std::uint32_t>(h->magic).load(std::memory_order_acquire) !=
            kMagic ||
        h->version != kVersion) {
      ::munmap(hp, granule);
      ::close(fd);
      return std::nullopt;
    }
    const std::uint64_t sq_cap = h->sq_capacity;
    const std::uint64_t cq_cap = h->cq_capacity;
    const std::uint64_t sq_off = h->sq_offset;
    const std::uint64_t cq_off = h->cq_offset;
    if (sq_off > file_size || sq_cap > file_size - sq_off ||
        cq_off > file_size || cq_cap > file_size - cq_off) {
      ::munmap(hp, granule);
      ::close(fd);
      return std::nullopt;
    }
    // The producer and consumer must agree on whether fillers exist, so open the ring
    // in exactly the mode create() chose. If the server mirror-mapped but this client
    // cannot (map_all returns false), open fails rather than risk a wrap/mirror
    // mismatch -- there is no safe single-map fallback against a mirror producer.
    const bool mirror = h->mirror != 0;
    ::munmap(hp, granule);  // remapped uniformly by map_all below
    Mapping m;
    if (!m.map_all(fd, sq_cap, cq_cap, sq_off, cq_off, granule, mirror)) {
      ::close(fd);
      return std::nullopt;
    }
    // Read-fault + lock: the server already allocated the backing, and a ring being
    // reconnected to may hold live records, so we must not write-touch here.
    m.prefault_and_lock(/*write_alloc=*/false);
    ::close(fd);
    return m;
  }

 private:
  // The allocation granule of the ring's backing store: the huge-page size when the
  // file lives on hugetlbfs (so the header, offsets, and mmap lengths must all be
  // huge-page multiples), else the base page. Detected from the open fd, so create()
  // and open() agree without a header field.
  static std::uint64_t detect_granule(int fd) noexcept {
#if defined(__linux__)
    struct statfs sfs;
    if (::fstatfs(fd, &sfs) == 0 &&
        (static_cast<std::uint64_t>(sfs.f_type) & 0xffffffffULL) ==
            static_cast<std::uint64_t>(HUGETLBFS_MAGIC)) {
      return static_cast<std::uint64_t>(sfs.f_bsize);
    }
#else
    (void)fd;
#endif
    return page_size();
  }

  static std::string base_name(const char* path) {
    const std::string p(path);
    const auto slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
  }

  // Reserve `len` bytes of contiguous address space (no backing) to hold the two
  // halves of a mirror map before MAP_FIXED drops the file over them. MAP_ANON is not
  // POSIX and glibc hides it under __USE_MISC (off in strict -std=c++23), so prefer it
  // when the macro is visible and otherwise reserve via a MAP_PRIVATE map of
  // /dev/zero -- the pre-MAP_ANON portable idiom.
  static void* reserve_va(std::uint64_t len) noexcept {
#if defined(MAP_ANONYMOUS)
    return ::mmap(nullptr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
    return ::mmap(nullptr, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    const int z = ::open("/dev/zero", O_RDWR);
    if (z < 0) {
      return MAP_FAILED;
    }
    void* p = ::mmap(nullptr, len, PROT_NONE, MAP_PRIVATE, z, 0);
    ::close(z);
    return p;
#endif
  }

  // Single file-backed mapping of [off, off+cap). Returns nullptr on failure.
  static char* map_single(int fd, std::uint64_t off, std::uint64_t cap) noexcept {
    void* p = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     static_cast<off_t>(off));
    return p == MAP_FAILED ? nullptr : static_cast<char*>(p);
  }

  // Mirror mapping: reserve 2*cap of address space, then MAP_FIXED the file's
  // [off, off+cap) over BOTH halves, so the bytes at virtual offset i and i+cap are
  // the same file byte. A record placed near the end can then be read/written as one
  // contiguous span running past `cap` into the mirror. `granule` is the mmap
  // alignment: MAP_FIXED of a hugetlbfs file needs a huge-page-aligned address, so we
  // over-reserve by one granule and align the base up (a no-op when granule == the
  // base page). Returns nullptr (leaving nothing mapped) on failure.
  static char* map_mirror(int fd, std::uint64_t off, std::uint64_t cap,
                          std::uint64_t granule) noexcept {
    const bool align = granule > page_size();
    const std::uint64_t reserve = align ? 2 * cap + granule : 2 * cap;
    void* raw = reserve_va(reserve);
    if (raw == MAP_FAILED) {
      return nullptr;
    }
    char* rbeg = static_cast<char*>(raw);
    char* b = rbeg;
    if (align) {
      const std::uint64_t off_in = reinterpret_cast<std::uintptr_t>(rbeg) & (granule - 1);
      b = off_in == 0 ? rbeg : rbeg + (granule - off_in);
      // Trim the slack outside [b, b+2cap) so we do not leak reserved address space.
      if (b > rbeg) {
        ::munmap(rbeg, static_cast<std::size_t>(b - rbeg));
      }
      char* bend = b + 2 * cap;
      char* rend = rbeg + reserve;
      if (rend > bend) {
        ::munmap(bend, static_cast<std::size_t>(rend - bend));
      }
    }
    void* lo = ::mmap(b, cap, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                      static_cast<off_t>(off));
    void* hi = ::mmap(b + cap, cap, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                      fd, static_cast<off_t>(off));
    if (lo != b || hi != b + cap) {
      ::munmap(b, 2 * cap);  // frees whatever of the two halves exists
      return nullptr;
    }
    return b;
  }

  bool map_all(int fd, std::uint64_t sq_cap, std::uint64_t cq_cap,
               std::uint64_t sq_off, std::uint64_t cq_off, std::uint64_t granule,
               bool mirror) noexcept {
    char* hp = map_single(fd, 0, granule);
    char* sq = mirror ? map_mirror(fd, sq_off, sq_cap, granule)
                      : map_single(fd, sq_off, sq_cap);
    char* cq = mirror ? map_mirror(fd, cq_off, cq_cap, granule)
                      : map_single(fd, cq_off, cq_cap);
    if (hp == nullptr || sq == nullptr || cq == nullptr) {
      if (hp != nullptr) {
        ::munmap(hp, granule);
      }
      if (sq != nullptr) {
        ::munmap(sq, mirror ? 2 * sq_cap : sq_cap);
      }
      if (cq != nullptr) {
        ::munmap(cq, mirror ? 2 * cq_cap : cq_cap);
      }
      return false;
    }
    header_ = reinterpret_cast<Header*>(hp);
    sq_ = sq;
    cq_ = cq;
    sq_cap_ = sq_cap;
    cq_cap_ = cq_cap;
    granule_ = granule;
    mirror_ = mirror;
    return true;
  }

  // Touch one byte of every page so the whole region is resident before the first
  // op -- the alternative is a minor page fault per page on the first lap around the
  // ring, i.e. µs-scale jitter sprinkled through the initial traffic. `write` stores
  // a byte (which also allocates the sparse file's backing blocks, so a later store
  // can never SIGBUS on an unbacked page); otherwise it only reads (faulting this
  // process's page-table entries without modifying shared data).
  static void fault_pages(void* addr, std::uint64_t len, bool write,
                          std::uint64_t stride) noexcept {
    volatile char* p = static_cast<volatile char*>(addr);
    if (write) {
      for (std::uint64_t i = 0; i < len; i += stride) p[i] = 0;
    } else {
      volatile char sink = 0;
      for (std::uint64_t i = 0; i < len; i += stride) sink = p[i];
      (void)sink;
    }
  }

  // Prefault every mapped region and lock it resident (mlock) so the ring never
  // takes a page fault -- neither a first-touch fault nor a later reclaim/swap -- on
  // the hot path. Locking is best-effort: a low RLIMIT_MEMLOCK or missing privilege
  // must not stop the ring from working, and by this point the touch loop has already
  // made the pages present, so an mlock failure only forfeits the never-evicted
  // guarantee, not correctness. (munmap drops the locks, so unmap needs no munlock.)
  //
  // `write_alloc` write-faults (allocating backing). Only the server that create()s
  // the ring passes true, before it publishes the magic -- nothing else is mapped
  // yet. A client open()ing an existing ring passes false: the blocks are already
  // allocated, and a ring being reconnected to may hold live records the server
  // produced, which a write-touch would clobber.
  void prefault_and_lock(bool write_alloc) noexcept {
    // Fault every mapped page (both halves in mirror mode) so the first lap takes no
    // minor faults; stride by the granule (base page, or one touch per huge page).
    // Lock only the first `cap` of each ring: in mirror mode the second half maps the
    // same physical pages, so pinning the first copy keeps them all resident without
    // double-counting against RLIMIT_MEMLOCK.
    fault_pages(header_, granule_, write_alloc, granule_);
    fault_pages(sq_, mirror_ ? 2 * sq_cap_ : sq_cap_, write_alloc, granule_);
    fault_pages(cq_, mirror_ ? 2 * cq_cap_ : cq_cap_, write_alloc, granule_);
    (void)::mlock(header_, granule_);
    (void)::mlock(sq_, sq_cap_);
    (void)::mlock(cq_, cq_cap_);
  }

  void unmap() noexcept {
    if (header_ != nullptr) {
      ::munmap(header_, granule_);
    }
    if (sq_ != nullptr) {
      ::munmap(sq_, mirror_ ? 2 * sq_cap_ : sq_cap_);
    }
    if (cq_ != nullptr) {
      ::munmap(cq_, mirror_ ? 2 * cq_cap_ : cq_cap_);
    }
    header_ = nullptr;
    sq_ = nullptr;
    cq_ = nullptr;
  }

  // Unlink the files this Mapping owns (a hugetlb ring's real file + its symlink),
  // freeing the huge pages. Empty for a normal ring, so it leaves /tmp ring files be,
  // matching prior behavior. Called after unmap so the file is no longer mapped here.
  void unlink_files() noexcept {
    if (!link_path_.empty()) {
      ::unlink(link_path_.c_str());
      link_path_.clear();
    }
    if (!real_path_.empty()) {
      ::unlink(real_path_.c_str());
      real_path_.clear();
    }
  }

  void reset() noexcept {
    unmap();
    unlink_files();
  }

  void move_from(Mapping& other) noexcept {
    header_ = other.header_;
    sq_ = other.sq_;
    cq_ = other.cq_;
    sq_cap_ = other.sq_cap_;
    cq_cap_ = other.cq_cap_;
    granule_ = other.granule_;
    mirror_ = other.mirror_;
    real_path_ = std::move(other.real_path_);
    link_path_ = std::move(other.link_path_);
    other.header_ = nullptr;
    other.sq_ = nullptr;
    other.cq_ = nullptr;
    other.real_path_.clear();
    other.link_path_.clear();
  }

  Header* header_ = nullptr;
  char* sq_ = nullptr;
  char* cq_ = nullptr;
  std::uint64_t sq_cap_ = 0;
  std::uint64_t cq_cap_ = 0;
  std::uint64_t granule_ = 0;  // base page, or huge-page size on hugetlbfs
  bool mirror_ = false;
  std::string real_path_;  // hugetlb: the real file on the mount (owned; unlinked)
  std::string link_path_;  // hugetlb: the user-facing symlink (owned; unlinked)
};

}  // namespace goblin::core::ring
