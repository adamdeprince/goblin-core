#pragma once

// NUMA placement via raw syscalls -- deliberately no libnuma (it is not installed on
// the multi-node test box, and mbind/set_mempolicy/move_pages are plain syscalls).
// Linux-only; on every other platform each call is a no-op that reports "unavailable",
// so callers degrade cleanly.
//
// Two policies the server uses, matching the two very different needs:
//   * The ring wants STRICT local placement (mbind MPOL_BIND). A ring on a remote node
//     defeats its whole reason to exist -- the sub-microsecond cache-line handoff -- so
//     failing to place it on the pinned CPU's node is a fatal startup error, not a
//     silent slowdown.
//   * The arenas want SOFT local preference (set_mempolicy MPOL_PREFERRED): best-effort,
//     spills to other nodes when the local one is full. Opt-in (--numa-arena) because
//     pinning all of a large server's memory to one node can starve clients co-located
//     on it.

#include <cstddef>

#include <sched.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <cstdio>
#include <cstdint>
#endif

namespace goblin::core::numa {

#if defined(__linux__)

// Linux mempolicy modes / flags (from linux/mempolicy.h; defined here to avoid a
// libnuma/uapi header dependency).
inline constexpr int kMpolDefault = 0;
inline constexpr int kMpolPreferred = 1;
inline constexpr int kMpolBind = 2;
inline constexpr unsigned long kMaxNodes = 64;  // one machine-word node mask

// Is `cpu` a member of a "0,4,8-12,16" style cpulist?
[[nodiscard]] inline bool cpu_in_list(const char* list, int cpu) noexcept {
  for (const char* p = list; *p != '\0';) {
    if (*p < '0' || *p > '9') {
      ++p;
      continue;
    }
    int lo = 0;
    while (*p >= '0' && *p <= '9') lo = lo * 10 + (*p++ - '0');
    int hi = lo;
    if (*p == '-') {
      ++p;
      hi = 0;
      while (*p >= '0' && *p <= '9') hi = hi * 10 + (*p++ - '0');
    }
    if (cpu >= lo && cpu <= hi) return true;
  }
  return false;
}

// The NUMA node that owns `cpu`, or -1 if it cannot be determined. Reads
// /sys/devices/system/node/node<K>/cpulist for each present node.
[[nodiscard]] inline int node_of_cpu(int cpu) noexcept {
  for (int node = 0; node < 1024; ++node) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/node/node%d/cpulist", node);
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) {
      return -1;  // no node<node> dir: past the last node
    }
    char line[8192];
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (got && cpu_in_list(line, cpu)) {
      return node;
    }
  }
  return -1;
}

// Pin the current thread (and threads it later spawns) to `cpu`. False on failure.
[[nodiscard]] inline bool pin_to_cpu(int cpu) noexcept {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

// Restrict the current thread (and threads it later spawns) to every CPU in one
// NUMA node. --cpu remains the exact-core control; this is the slice-level form.
[[nodiscard]] inline bool pin_to_node(int node) noexcept {
  char path[128];
  std::snprintf(path, sizeof(path),
                "/sys/devices/system/node/node%d/cpulist", node);
  std::FILE* f = std::fopen(path, "re");
  if (f == nullptr) return false;
  char line[8192];
  const bool got = std::fgets(line, sizeof(line), f) != nullptr;
  std::fclose(f);
  if (!got) return false;

  cpu_set_t set;
  CPU_ZERO(&set);
  bool any = false;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (cpu_in_list(line, cpu)) {
      CPU_SET(cpu, &set);
      any = true;
    }
  }
  return any && ::sched_setaffinity(0, sizeof(set), &set) == 0;
}

// STRICT-bind [addr, len) to `node` (mbind MPOL_BIND): pages faulted in after this
// call come only from `node`. Call BEFORE first-touch. No MPOL_MF_STRICT flag -- the
// region is not yet populated, and STRICT would reject already-present pages. Used for
// the ring, whose placement is then verified with all_on_node(). False on syscall error.
[[nodiscard]] inline bool bind_range(void* addr, std::size_t len, int node) noexcept {
  if (node < 0 || static_cast<unsigned>(node) >= kMaxNodes) return false;
  const unsigned long mask = 1UL << static_cast<unsigned>(node);
  return ::syscall(SYS_mbind, addr, len, kMpolBind, &mask, kMaxNodes, 0) == 0;
}

// Set the process default policy to PREFER `node` (soft; spills elsewhere if full).
// Governs later allocations, so the arenas that grow at runtime land on `node` when
// they can. False on syscall error.
[[nodiscard]] inline bool prefer_node(int node) noexcept {
  if (node < 0 || static_cast<unsigned>(node) >= kMaxNodes) return false;
  const unsigned long mask = 1UL << static_cast<unsigned>(node);
  return ::syscall(SYS_set_mempolicy, kMpolPreferred, &mask, kMaxNodes) == 0;
}

// Set the process default policy to STRICTLY bind to `node` (MPOL_BIND). Used to
// bracket ring creation: the ring's prefault then allocates only from `node` (or the
// allocation fails), so the ring is local by construction; restore afterwards with
// prefer_node()/reset_policy(). False on syscall error.
[[nodiscard]] inline bool bind_process_to_node(int node) noexcept {
  if (node < 0 || static_cast<unsigned>(node) >= kMaxNodes) return false;
  const unsigned long mask = 1UL << static_cast<unsigned>(node);
  return ::syscall(SYS_set_mempolicy, kMpolBind, &mask, kMaxNodes) == 0;
}

// Clear the process policy back to the system default (allocate on the faulting CPU's
// node, spilling as needed). Used to un-bracket after ring creation when the arenas
// are not being pinned.
inline void reset_policy() noexcept {
  (void)::syscall(SYS_set_mempolicy, kMpolDefault, nullptr, kMaxNodes);
}

// Verify every resident page of [addr, len) is on `node` (move_pages query mode: null
// target nodes => it fills `status` with each page's current node). Touch the region
// first. Returns false if any page is elsewhere or the query fails -- this is what
// turns a would-be mid-run remote access (or a hugetlb SIGBUS) into a clean startup
// abort for the ring.
[[nodiscard]] inline bool all_on_node(void* addr, std::size_t len, int node) noexcept {
  const auto pg = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  auto base = reinterpret_cast<std::uintptr_t>(addr);
  const std::uintptr_t end = base + len;
  constexpr std::size_t kBatch = 512;
  void* pages[kBatch];
  int status[kBatch];
  while (base < end) {
    std::size_t n = 0;
    for (; n < kBatch && base < end; ++n, base += pg) {
      pages[n] = reinterpret_cast<void*>(base);
      status[n] = -1;
    }
    const long r = ::syscall(SYS_move_pages, 0, n, pages, nullptr, status, 0);
    if (r != 0) return false;
    for (std::size_t i = 0; i < n; ++i) {
      if (status[i] != node) return false;
    }
  }
  return true;
}

#else  // not Linux: NUMA control is unavailable, so report so and no-op.

[[nodiscard]] inline int node_of_cpu(int) noexcept { return -1; }
[[nodiscard]] inline bool pin_to_cpu(int) noexcept { return false; }
[[nodiscard]] inline bool pin_to_node(int) noexcept { return false; }
[[nodiscard]] inline bool bind_range(void*, std::size_t, int) noexcept { return false; }
[[nodiscard]] inline bool prefer_node(int) noexcept { return false; }
[[nodiscard]] inline bool bind_process_to_node(int) noexcept { return false; }
inline void reset_policy() noexcept {}
[[nodiscard]] inline bool all_on_node(void*, std::size_t, int) noexcept { return false; }

#endif

}  // namespace goblin::core::numa
