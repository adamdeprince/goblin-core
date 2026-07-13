// Test for hugetlb-backed rings (Linux hugetlbfs). Skips cleanly (exit 0) when the
// platform is not Linux, no hugetlbfs is mounted, or no huge pages are reserved --
// so it never fails a build lacking huge pages. When pages are available it checks:
//   * the requested size rounds up to the huge page (1 MiB -> 2 MiB on x86);
//   * the ring is huge-backed and the --ring PATH is a symlink into hugetlbfs;
//   * a client opening the symlink path lands on the same huge-backed ring;
//   * records round-trip, including across the ring seam when mirror-mapped;
//   * destroying the server mapping unlinks both the symlink and the real file.

#include "goblin/core/ring_buffer.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace goblin::core::ring;  // NOLINT

int main() {
#if !defined(__linux__)
  std::puts("ring hugetlb: not Linux; skipped");
  return 0;
#else
  const std::uint64_t huge = detail::smallest_hugepage_over_base();
  if (huge == 0 || detail::find_hugetlbfs_mount(huge).empty()) {
    std::puts("ring hugetlb: no hugetlbfs mount available; skipped");
    return 0;
  }

  const std::string link = "/tmp/goblin-huge-" + std::to_string(::getpid()) + ".ring";
  ::unlink(link.c_str());

  // Request 1 MiB: with a 2 MiB huge page this must round up to the huge page.
  auto server = Mapping::create_hugetlb(link.c_str(), 1024 * 1024);
  if (!server) {
    std::puts("ring hugetlb: no huge pages reserved (HugePages_Free=0?); skipped");
    ::unlink(link.c_str());
    return 0;
  }

  // Huge-backed, size rounded up to the huge page, still a power of two.
  assert(server->is_hugetlb());
  assert(server->granule() == huge);
  assert(server->sq_capacity() >= huge && server->sq_capacity() >= 1024 * 1024);
  assert((server->sq_capacity() & (server->sq_capacity() - 1)) == 0);

  // The user path is a symlink pointing into hugetlbfs.
  struct stat st;
  assert(::lstat(link.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
  char target[4096];
  const ssize_t tn = ::readlink(link.c_str(), target, sizeof(target) - 1);
  assert(tn > 0);
  target[tn] = '\0';
  const std::string real(target);
  assert(real.find(detail::find_hugetlbfs_mount(huge)) == 0 &&
         "symlink must point into the hugetlbfs mount");

  // A client opens by the symlink path and lands on the same huge-backed ring.
  auto client = Mapping::open(link.c_str());
  assert(client && "client must open the ring through the symlink");
  assert(client->is_hugetlb() && client->granule() == huge);

  // Round-trip a record: client produces on the SQ, server consumes.
  Producer sq = client->sq_producer();
  Consumer rx = server->sq_consumer();
  const std::string payload(4000, 'H');  // spans several base pages
  assert(sq.try_push(payload));
  const auto got = rx.peek();
  assert(got && *got == payload);
  rx.pop();

  // If the huge-aligned mirror map took, drive traffic across the (huge) seam and
  // confirm each straddling record folds back and round-trips byte-for-byte.
  if (server->mirror()) {
    const std::uint64_t cap = server->sq_capacity();
    const std::uint64_t mask = cap - 1;
    const std::size_t len = 60000;  // padded ~64 KiB; ~32 records per 2 MiB lap
    std::uint64_t pos = 0;
    std::size_t straddles = 0;
    for (int i = 0; i < 128; ++i) {
      const std::uint64_t off = pos & mask;
      if (off + kCacheLine + len > cap) ++straddles;
      std::string rec(len, static_cast<char>('a' + (i % 26)));
      assert(sq.try_push(rec));
      const auto r = rx.peek();
      assert(r && r->size() == len && *r == rec &&
             "record must round-trip across the huge-page seam");
      rx.pop();
      pos += kCacheLine + align_up(len, kCacheLine);
    }
    assert(straddles > 0 && "traffic should cross the seam");
    std::printf("hugetlb mirror active (%llu-byte granule); %zu seam-straddling records ok\n",
                static_cast<unsigned long long>(huge), straddles);
  } else {
    std::printf("hugetlb single-map (%llu-byte granule); round-trip ok\n",
                static_cast<unsigned long long>(huge));
  }

  // Teardown: client unmaps (owns no files); server unmaps AND unlinks both files.
  client.reset();
  server.reset();
  assert(::access(link.c_str(), F_OK) != 0 && "symlink must be unlinked on destroy");
  assert(::access(real.c_str(), F_OK) != 0 && "real hugetlbfs file must be unlinked");

  std::puts("ring hugetlb OK");
  return 0;
#endif
}
