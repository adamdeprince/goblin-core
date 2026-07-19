// RESP variant of the ring reconnect test. A RESP RingClient that dies abruptly (SIGKILL)
// while attached to a ring must not wedge it: a fresh RingClient has to reconnect and drive
// the SAME ring with NO server restart. The corpse leaves the ring dirty on purpose --
// unread replies stranded in the CQ AND a half-sent command left in the server's RESP
// parser -- then is SIGKILLed holding the mapping. Recovery rides the same epoch handshake
// as the SBE path (RingClient::open bumps the epoch; the server drains + re-arms + acks).
//
//   ring_reconnect_test <path-to-goblin-core>

#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/ring_client.hpp"
#include "socket_test_utils.hpp"

#include <csignal>  // kill / SIGKILL / SIGTERM (not transitively via <sys/wait.h> on macOS)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace ring = goblin::core::ring;
using ring::RingClient;

// Push one full RESP command onto the SQ and never read its reply, so the server's reply
// is left stranded in the CQ.
void fire(ring::Producer& sq, std::span<const std::string_view> args) {
  sq.send(ring::encode_command(args), [] { return false; });
}

// Child process: attach, make a mess (unread replies + a truncated command), wait to die.
[[noreturn]] void crashing_client(const char* ring_path, int ready_fd) {
  auto m = ring::Mapping::open(ring_path);
  if (!m) _exit(3);
  const std::uint64_t epoch = m->request_reconnect();
  while (!m->reconnect_acked(epoch)) ring::cpu_relax();
  ring::Producer sq = m->sq_producer();
  for (int i = 0; i < 16; ++i) {
    const std::string_view args[] = {"SET", "hammer", "x"};
    fire(sq, args);  // 16 +OK replies nobody will read
  }
  // A deliberately truncated command: the server buffers it in its RESP parser with no
  // complete command to dispatch. If the reconnect did not clear the parser, the next
  // client's first command would glue onto this and mis-parse.
  sq.send(std::string_view("*3\r\n$3\r\nSET\r\n$6\r\nhalf"), [] { return false; });
  const char b = 'x';
  while (::write(ready_fd, &b, 1) < 0 && errno == EINTR) {
  }
  for (;;) ::pause();  // hold the mapping open and wait for SIGKILL
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: ring_reconnect_test <goblin-core>\n");
    return 2;
  }
  const char* server = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string ring_path = "/tmp/gcrespreconnect-" + tag + ".ring";
  const std::string sock = "/tmp/gcrespreconnect-" + tag + ".sock";
  const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
  assert(tcp_port != 0);
  const std::string tcp_port_text = std::to_string(tcp_port);
  ::unlink(ring_path.c_str());

  const pid_t srv = ::fork();
  assert(srv >= 0);
  if (srv == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
    ::execl(server, server, "--unixsocket", sock.c_str(), "--port",
            tcp_port_text.c_str(), "--ring", ring_path.c_str(), "1mb",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  // A first, well-behaved RESP client establishes state and disconnects cleanly.
  {
    auto c = RingClient::open(ring_path.c_str(), std::chrono::seconds(5));
    assert(c);
    assert(c->command({"PING"}) == "+PONG\r\n");
    (void)c->command({"SET", "survivor", "1"});
    assert(c->command({"GET", "survivor"}) == "$1\r\n1\r\n");
  }  // clean disconnect -- the next client must still be able to reconnect

  // A second RESP client crashes MESSILY: attaches, floods commands it never reads plus a
  // truncated command, and is SIGKILLed while still holding the mapping.
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
  // Give the server a moment to have buffered the truncated command in its parser, then
  // kill the client abruptly with no cleanup.
  ::usleep(100 * 1000);
  ::kill(bad, SIGKILL);
  ::waitpid(bad, nullptr, 0);

  // A third RESP client must recover the SAME ring with NO server restart.
  auto c = RingClient::open(ring_path.c_str(), std::chrono::seconds(5));
  assert(c);
  // The corpse left 16 unread "+OK" replies and a half-parsed command. If the drain and
  // parser re-arm had failed, THIS would return a stale "+OK" (or a parse error) instead of
  // our bulk string -> proof we read our OWN reply and the store survived.
  assert(c->command({"GET", "survivor"}) == "$1\r\n1\r\n");
  assert(c->command({"PING"}) == "+PONG\r\n");
  (void)c->command({"SET", "after", "ok"});
  assert(c->command({"GET", "after"}) == "$2\r\nok\r\n");
  assert(c->command({"INCR", "n"}) == ":1\r\n");
  assert(c->command({"INCR", "n"}) == ":2\r\n");

  ::kill(srv, SIGTERM);
  ::waitpid(srv, nullptr, 0);
  ::unlink(ring_path.c_str());
  ::unlink(sock.c_str());
  std::puts(
      "resp ring reconnect OK: a SIGKILLed RESP client left the ring dirty (unread replies "
      "+ a half-parsed command); a fresh client recovered the same ring with no restart");
  return 0;
}
