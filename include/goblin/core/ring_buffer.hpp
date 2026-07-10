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

#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace goblin::core::ring {

inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::uint32_t kMagic = 0x474E5247;  // 'GRNG'
inline constexpr std::uint32_t kVersion = 1;

// The system page size. mmap file offsets must be a multiple of it, so the header
// region and every ring capacity are aligned to it. It is 4 KiB on x86, but 16 KiB
// on Apple Silicon and configurable on other ARM/LoongArch systems, so it must be
// read at runtime rather than assumed.
[[nodiscard]] inline std::uint64_t page_size() noexcept {
  static const std::uint64_t ps = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  return ps;
}

// The header occupies one page; the SQ and CQ data regions follow it.
[[nodiscard]] inline std::uint64_t header_bytes() noexcept { return page_size(); }

// Spin-loop relax: hint the core to back off so a sibling hyperthread (and the
// remote core writing our pages) makes progress, and so a busy-poll does not burn
// the pipeline on failed speculation. The atomics carry the ordering; this is a
// throughput/power hint only.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
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
struct alignas(kCacheLine) Header {
  std::uint32_t magic;       // written last on create; readers gate on it
  std::uint32_t version;
  std::uint64_t sq_capacity;  // power of two
  std::uint64_t cq_capacity;  // power of two
  std::uint64_t sq_offset;    // file offset of SQ data
  std::uint64_t cq_offset;    // file offset of CQ data
  std::uint64_t reserved;
  // Each index on its own cache line: the producer writes one, the consumer the
  // other, and they must not share a line or every publish bounces the cache.
  alignas(kCacheLine) std::uint64_t sq_head;  // consumer (server) owns
  alignas(kCacheLine) std::uint64_t sq_tail;  // producer (client) owns
  alignas(kCacheLine) std::uint64_t cq_head;  // consumer (client) owns
  alignas(kCacheLine) std::uint64_t cq_tail;  // producer (server) owns
};
static_assert(sizeof(Header) == 5 * kCacheLine);
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
class Consumer {
 public:
  Consumer() = default;
  Consumer(std::uint64_t* head, std::uint64_t* tail, char* data,
           std::uint64_t capacity) noexcept
      : head_(head), tail_(tail), data_(data), capacity_(capacity),
        mask_(capacity - 1) {}

  // Is there a published data record to read?
  [[nodiscard]] bool has_record() const noexcept { return advance_to_record(); }

  // The next record's payload, as a contiguous view into the ring, or nullopt
  // when empty. Valid until pop().
  [[nodiscard]] std::optional<std::string_view> peek() const noexcept {
    if (!advance_to_record()) {
      return std::nullopt;
    }
    const std::uint64_t h = load_head();
    const char* base = data_ + (h & mask_);
    const std::uint32_t len = detail::get_u32(base);
    return std::string_view(base + kCacheLine, len);
  }

  // Discard the data record most recently peek()'d, freeing its space.
  void pop() const noexcept {
    const std::uint64_t h = load_head();
    const std::uint32_t len = detail::get_u32(data_ + (h & mask_));
    store_head(h + kCacheLine + align_up(len, kCacheLine));
  }

 private:
  [[nodiscard]] std::uint64_t load_head() const noexcept {
    return std::atomic_ref<std::uint64_t>(*head_).load(std::memory_order_relaxed);
  }
  void store_head(std::uint64_t v) const noexcept {
    std::atomic_ref<std::uint64_t>(*head_).store(v, std::memory_order_release);
  }
  // Advance past any WRAP fillers; true once a data record sits at head.
  [[nodiscard]] bool advance_to_record() const noexcept {
    for (;;) {
      const std::uint64_t h = load_head();
      const std::uint64_t t = std::atomic_ref<std::uint64_t>(*tail_).load(
          std::memory_order_acquire);
      if (h == t) {
        return false;
      }
      const std::uint64_t off = h & mask_;
      if (detail::get_u32(data_ + off + sizeof(std::uint32_t)) == kFlagWrap) {
        store_head(h + (capacity_ - off));  // jump to the ring boundary (offset 0)
        continue;
      }
      return true;
    }
  }

  std::uint64_t* head_ = nullptr;
  std::uint64_t* tail_ = nullptr;
  char* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t mask_ = 0;
};

// A single-producer view over one direction. Writes payloads as cache-aligned
// records, splitting anything larger than a single record across several.
class Producer {
 public:
  Producer() = default;
  Producer(std::uint64_t* head, std::uint64_t* tail, char* data,
           std::uint64_t capacity) noexcept
      : head_(head), tail_(tail), data_(data), capacity_(capacity),
        mask_(capacity - 1),
        // A record uses one control line plus the padded payload; cap a single
        // record at half the ring so at least two can coexist (pipelining) and no
        // single record needs a fully-drained ring.
        max_payload_(capacity / 2 - kCacheLine) {}

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
    std::uint64_t t = std::atomic_ref<std::uint64_t>(*tail_).load(
        std::memory_order_relaxed);
    const std::uint64_t h = std::atomic_ref<std::uint64_t>(*head_).load(
        std::memory_order_acquire);
    const std::uint64_t off = t & mask_;
    const std::uint64_t to_end = capacity_ - off;
    const std::uint64_t need = padded > to_end ? to_end + padded : padded;
    if (capacity_ - (t - h) < need) {
      return false;  // not enough free space (counting any wrap filler)
    }
    if (padded > to_end) {
      // Record will not fit before the end: drop a WRAP filler that fills to the
      // boundary, then continue at offset 0.
      char* filler = data_ + off;
      detail::put_u32(filler, 0);
      detail::put_u32(filler + sizeof(std::uint32_t), kFlagWrap);
      t += to_end;  // now (t & mask_) == 0
    }
    char* base = data_ + (t & mask_);
    detail::put_u32(base, static_cast<std::uint32_t>(payload.size()));
    detail::put_u32(base + sizeof(std::uint32_t), kFlagData);
    std::memcpy(base + kCacheLine, payload.data(), payload.size());
    std::atomic_ref<std::uint64_t>(*tail_).store(t + padded,
                                                 std::memory_order_release);
    return true;
  }

  // Enqueue all of `payload`, splitting into records and spinning (cpu_relax) for
  // space between them. `stop()` aborts the wait (returns early) -- e.g. on
  // shutdown or a client-side timeout.
  template <class StopFn>
  void send(std::string_view payload, StopFn&& stop) noexcept {
    while (!payload.empty()) {
      const std::size_t chunk =
          payload.size() < max_payload_ ? payload.size() : max_payload_;
      const std::string_view piece = payload.substr(0, chunk);
      while (!try_push(piece)) {
        cpu_relax();
        if (stop()) {
          return;
        }
      }
      payload.remove_prefix(chunk);
    }
  }

 private:
  std::uint64_t* head_ = nullptr;
  std::uint64_t* tail_ = nullptr;
  char* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  std::uint64_t mask_ = 0;
  std::uint64_t max_payload_ = 0;
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
      unmap();
      move_from(other);
    }
    return *this;
  }
  ~Mapping() { unmap(); }

  [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }
  [[nodiscard]] Header* header() const noexcept { return header_; }
  [[nodiscard]] std::uint64_t sq_capacity() const noexcept { return sq_cap_; }
  [[nodiscard]] std::uint64_t cq_capacity() const noexcept { return cq_cap_; }

  // Server side: consume the SQ (requests), produce into the CQ (replies).
  [[nodiscard]] Consumer sq_consumer() const noexcept {
    return Consumer(&header_->sq_head, &header_->sq_tail, sq_, sq_cap_);
  }
  [[nodiscard]] Producer cq_producer() const noexcept {
    return Producer(&header_->cq_head, &header_->cq_tail, cq_, cq_cap_);
  }
  // Client side: produce into the SQ (requests), consume the CQ (replies).
  [[nodiscard]] Producer sq_producer() const noexcept {
    return Producer(&header_->sq_head, &header_->sq_tail, sq_, sq_cap_);
  }
  [[nodiscard]] Consumer cq_consumer() const noexcept {
    return Consumer(&header_->cq_head, &header_->cq_tail, cq_, cq_cap_);
  }

  // Create (or re-create) a ring file and initialize it. The server calls this.
  // `sq_cap`/`cq_cap` must be powers of two >= one page.
  [[nodiscard]] static std::optional<Mapping> create(const char* path,
                                                     std::uint64_t sq_cap,
                                                     std::uint64_t cq_cap) noexcept {
    const int fd = ::open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
      return std::nullopt;
    }
    const std::uint64_t total = header_bytes() + sq_cap + cq_cap;
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
      ::close(fd);
      return std::nullopt;
    }
    Mapping m;
    if (!m.map_all(fd, sq_cap, cq_cap, header_bytes(),
                   header_bytes() + sq_cap)) {
      ::close(fd);
      return std::nullopt;
    }
    Header* h = m.header_;
    h->version = kVersion;
    h->sq_capacity = sq_cap;
    h->cq_capacity = cq_cap;
    h->sq_offset = header_bytes();
    h->cq_offset = header_bytes() + sq_cap;
    h->reserved = 0;
    std::atomic_ref<std::uint64_t>(h->sq_head).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->sq_tail).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->cq_head).store(0, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t>(h->cq_tail).store(0, std::memory_order_relaxed);
    // Publish magic last so a reader that gates on it sees a fully-init header.
    std::atomic_ref<std::uint32_t>(h->magic).store(kMagic, std::memory_order_release);
    ::close(fd);
    return m;
  }

  // Open an existing, initialized ring file. The client calls this. Returns
  // nullopt if the file is missing, not yet initialized (magic unset), or the
  // version mismatches.
  [[nodiscard]] static std::optional<Mapping> open(const char* path) noexcept {
    const int fd = ::open(path, O_RDWR);
    if (fd < 0) {
      return std::nullopt;
    }
    void* hp = ::mmap(nullptr, header_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
    if (hp == MAP_FAILED) {
      ::close(fd);
      return std::nullopt;
    }
    Header* h = static_cast<Header*>(hp);
    if (std::atomic_ref<std::uint32_t>(h->magic).load(std::memory_order_acquire) !=
            kMagic ||
        h->version != kVersion) {
      ::munmap(hp, header_bytes());
      ::close(fd);
      return std::nullopt;
    }
    const std::uint64_t sq_cap = h->sq_capacity;
    const std::uint64_t cq_cap = h->cq_capacity;
    const std::uint64_t sq_off = h->sq_offset;
    const std::uint64_t cq_off = h->cq_offset;
    ::munmap(hp, header_bytes());  // remapped uniformly by map_all below
    Mapping m;
    if (!m.map_all(fd, sq_cap, cq_cap, sq_off, cq_off)) {
      ::close(fd);
      return std::nullopt;
    }
    ::close(fd);
    return m;
  }

 private:
  bool map_all(int fd, std::uint64_t sq_cap, std::uint64_t cq_cap,
               std::uint64_t sq_off, std::uint64_t cq_off) noexcept {
    void* hp = ::mmap(nullptr, header_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
    void* sq = ::mmap(nullptr, sq_cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      static_cast<off_t>(sq_off));
    void* cq = ::mmap(nullptr, cq_cap, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                      static_cast<off_t>(cq_off));
    if (hp == MAP_FAILED || sq == MAP_FAILED || cq == MAP_FAILED) {
      if (hp != MAP_FAILED) {
        ::munmap(hp, header_bytes());
      }
      if (sq != MAP_FAILED) {
        ::munmap(sq, sq_cap);
      }
      if (cq != MAP_FAILED) {
        ::munmap(cq, cq_cap);
      }
      return false;
    }
    header_ = static_cast<Header*>(hp);
    sq_ = static_cast<char*>(sq);
    cq_ = static_cast<char*>(cq);
    sq_cap_ = sq_cap;
    cq_cap_ = cq_cap;
    return true;
  }

  void unmap() noexcept {
    if (header_ != nullptr) {
      ::munmap(header_, header_bytes());
    }
    if (sq_ != nullptr) {
      ::munmap(sq_, sq_cap_);
    }
    if (cq_ != nullptr) {
      ::munmap(cq_, cq_cap_);
    }
    header_ = nullptr;
    sq_ = nullptr;
    cq_ = nullptr;
  }

  void move_from(Mapping& other) noexcept {
    header_ = other.header_;
    sq_ = other.sq_;
    cq_ = other.cq_;
    sq_cap_ = other.sq_cap_;
    cq_cap_ = other.cq_cap_;
    other.header_ = nullptr;
    other.sq_ = nullptr;
    other.cq_ = nullptr;
  }

  Header* header_ = nullptr;
  char* sq_ = nullptr;
  char* cq_ = nullptr;
  std::uint64_t sq_cap_ = 0;
  std::uint64_t cq_cap_ = 0;
};

}  // namespace goblin::core::ring
