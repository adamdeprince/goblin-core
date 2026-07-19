// A client that dies abruptly (SIGKILL) while still attached to a ring must not wedge
// it: a brand-new client has to reconnect and drive the SAME ring with NO server
// restart. The dying client leaves the ring deliberately dirty -- it fires commands and
// never reads the replies, so the CQ is left full of unread replies (and the SQ may hold
// unconsumed requests) -- then is killed with SIGKILL, holding the mapping, no cleanup.
//
// This exercises the reconnect handshake: the newcomer bumps the ring epoch, the server
// drains the corpse's leftovers and re-arms protocol detection, then acks.
//
//   sbe_reconnect_test <path-to-goblin-core>

#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/sbe_ring_client.hpp"
#include "socket_test_utils.hpp"

#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/Ping.h"

#include <csignal>  // kill / SIGKILL / SIGTERM (not transitively via <sys/wait.h> on macOS)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ring = goblin::core::ring;
using goblin::core::kGoblinMagicBytes;
using goblin::core::kSbeLenPrefix;
using goblin::core::SbeRingClient;
namespace sbe = goblin_sbe;

// Fire one raw PING frame onto the SQ and DON'T read its reply, so the server's PONG is
// left stranded in the CQ. (We go under SbeRingClient here precisely because its typed
// methods always read their reply -- we want to leave a mess.)
void fire_ping(ring::Producer& sq, std::vector<char>& buf) {
  sbe::Ping m;
  m.wrapAndApplyHeader(buf.data(), kSbeLenPrefix, buf.size());
  const auto len =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + m.encodedLength());
  std::memcpy(buf.data(), &len, kSbeLenPrefix);
  sq.send(std::string_view(buf.data(), kSbeLenPrefix + len), [] { return false; });
}

// Child process: attach to the ring, make a mess, then wait to be killed. Never returns.
[[noreturn]] void crashing_client(const char* ring_path, int ready_fd) {
  auto m = ring::Mapping::open(ring_path);
  if (!m) _exit(3);
  // Reconnect handshake by hand (so that, unlike SbeRingClient, we can then leave replies
  // unread on purpose).
  const std::uint64_t epoch = m->request_reconnect();
  while (!m->reconnect_acked(epoch)) ring::cpu_relax();
  ring::Producer sq = m->sq_producer();
  sq.send(std::string_view(kGoblinMagicBytes, sizeof(kGoblinMagicBytes)), [] { return false; });
  std::vector<char> buf(256);
  for (int i = 0; i < 16; ++i) fire_ping(sq, buf);  // 16 replies nobody will ever read
  const char b = 'x';
  while (::write(ready_fd, &b, 1) < 0 && errno == EINTR) {
  }
  for (;;) ::pause();  // hold the mapping open and wait for SIGKILL
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: sbe_reconnect_test <goblin-core>\n");
    return 2;
  }
  const char* server = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string ring_path = "/tmp/gcreconnect-" + tag + ".ring";
  const std::string sock = "/tmp/gcreconnect-" + tag + ".sock";
  const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
  assert(tcp_port != 0);
  const std::string tcp_port_text = std::to_string(tcp_port);
  ::unlink(ring_path.c_str());

  const pid_t srv = ::fork();
  assert(srv >= 0);
  if (srv == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
    ::execl(server, server, "--enable-sbe", "--unixsocket", sock.c_str(),
            "--port", tcp_port_text.c_str(), "--ring", ring_path.c_str(),
            "1mb", static_cast<char*>(nullptr));
    _exit(127);
  }

  // A first, well-behaved client establishes some state and disconnects cleanly.
  {
    auto c = SbeRingClient::open(ring_path.c_str(), std::chrono::seconds(5));
    assert(c);
    assert(c->ping());
    (void)c->set("survivor", "1");
    assert(c->get("survivor") == "1");
  }  // clean disconnect (destructor) -- the next client must still be able to reconnect

  // A second client crashes MESSILY: it attaches, floods the ring with commands it never
  // reads, and is SIGKILLed while still holding the mapping.
  int ready[2];
  assert(::pipe(ready) == 0);
  const pid_t bad = ::fork();
  assert(bad >= 0);
  if (bad == 0) {
    ::close(ready[0]);
    crashing_client(ring_path.c_str(), ready[1]);
  }
  ::close(ready[1]);
  char b = 0;
  assert(::read(ready[0], &b, 1) == 1);  // wait until it has made its mess
  ::close(ready[0]);
  ::kill(bad, SIGKILL);  // abrupt, no cleanup: the ring is left dirty and the mapping stale
  ::waitpid(bad, nullptr, 0);

  // A third client must recover the SAME ring with NO server restart.
  auto c = SbeRingClient::open(ring_path.c_str(), std::chrono::seconds(5));
  assert(c);
  // The corpse left 16 unread PONGs (StatusReply) in the CQ. If the drain had failed,
  // THIS read would surface one of them instead of our BulkReply -> a type-mismatch throw,
  // so it doubles as proof that we read OUR reply, not a stale one -- and that the store
  // survived the crash intact.
  assert(c->get("survivor") == "1");
  assert(c->ping());
  (void)c->set("after", "ok");
  assert(c->get("after") == "ok");
  assert(c->incr("n") == 1 && c->incr("n") == 2);
  assert(c->echo("fresh") == "fresh");
  using S = std::vector<SbeRingClient::Scored>;
  assert(c->zadd("z", S{{1.0, "a"}, {2.0, "b"}}) == 2 && c->zcard("z") == 2);

  ::kill(srv, SIGTERM);
  ::waitpid(srv, nullptr, 0);
  ::unlink(ring_path.c_str());
  ::unlink(sock.c_str());
  std::puts(
      "sbe reconnect OK: a SIGKILLed client left the ring dirty (unread replies, stale "
      "mapping); a fresh client recovered the same ring with no server restart");
  return 0;
}
