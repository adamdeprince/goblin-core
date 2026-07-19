// Integration test: fork+exec the real goblin-core binary with a ring, then drive
// it end to end through the header-only RingClient (client.command -> reply). This
// exercises the whole shared-memory path across two processes, and doubles as a
// worked example of using the client as a test harness.
//
//   ring_roundtrip_test <path-to-goblin-core>

#include "goblin/core/auth.hpp"
#include "goblin/core/ring_client.hpp"

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using goblin::core::ring::RingClient;

std::string reply_of(RingClient& c, std::vector<std::string_view> cmd) {
  auto r = c.command(std::span<const std::string_view>(cmd.data(), cmd.size()));
  assert(r && "no reply from ring (server dead?)");
  return *r;
}

void expect(RingClient& c, std::vector<std::string_view> cmd, std::string_view want) {
  const std::string got = reply_of(c, cmd);
  if (got != want) {
    std::fprintf(stderr, "ring_roundtrip_test: for command, expected %.*s got %.*s\n",
                 static_cast<int>(want.size()), want.data(),
                 static_cast<int>(got.size()), got.data());
    std::abort();
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: ring_roundtrip_test <goblin-core-binary>\n");
    return 2;
  }
  const char* server_bin = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string ring_path = "/tmp/goblin-ring-rt-" + tag + ".ring";
  const std::string sock_path = "/tmp/goblin-ring-rt-" + tag + ".sock";
  const std::string auth_path = "/tmp/goblin-ring-rt-" + tag + ".auth";
  ::unlink(ring_path.c_str());
  ::unlink(sock_path.c_str());
  ::unlink(auth_path.c_str());
  ::unlink((auth_path + ".lock").c_str());
  assert(goblin::core::upsert_auth_user(auth_path, "default", "secret") ==
         goblin::core::AuthUserUpdate::added);

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    // Child: the server, with a ring and a unix socket for the (unused) network
    // fallback. Silence its startup chatter.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    ::execl(server_bin, server_bin, "--auth-file", auth_path.c_str(),
            "--no-auth-ring", "--unixsocket", sock_path.c_str(), "--ring",
            ring_path.c_str(), "64kb", static_cast<char*>(nullptr));
    _exit(127);
  }

  int rc = 0;
  {
    auto client = RingClient::open(ring_path.c_str(), std::chrono::seconds(5));
    assert(client && "could not open ring (server failed to start?)");

    // Basic request/response across the ring.
    expect(*client, {"PING"}, "+PONG\r\n");
    // The trusted-transport bypass makes AUTH optional, not unavailable.
    expect(*client, {"AUTH", "secret"}, "+OK\r\n");
    expect(*client, {"SET", "foo", "bar"}, "+OK\r\n");
    expect(*client, {"GET", "foo"}, "$3\r\nbar\r\n");
    expect(*client, {"GET", "missing"}, "$-1\r\n");
    expect(*client, {"INCR", "n"}, ":1\r\n");
    expect(*client, {"INCR", "n"}, ":2\r\n");

    // A native command and a zset, proving the full command surface is reachable.
    expect(*client, {"GOBLIN.INCRBOUND", "q", "3", "10"}, ":3\r\n");
    expect(*client, {"ZADD", "z", "1", "a", "2", "b"}, ":2\r\n");
    expect(*client, {"ZSCORE", "z", "b"}, "$1\r\n2\r\n");
    expect(*client, {"TYPE", "wrong"}, "+none\r\n");
    expect(*client, {"ZADD", "foo", "1", "x"},
           "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    // RESP negotiation is connection-local and may happen after ordinary RESP2
    // traffic. Pipeline a command behind HELLO to prove the new version applies
    // immediately to the next buffered command.
    client->send(std::vector<std::string_view>{"HELLO", "3"});
    client->send(std::vector<std::string_view>{"GET", "missing-resp3"});
    const auto hello3 = client->read_reply();
    assert(hello3 && hello3->starts_with("%7\r\n"));
    assert(hello3->find("$5\r\nproto\r\n:3\r\n") != std::string::npos);
    const auto null3 = client->read_reply();
    assert(null3 && *null3 == "_\r\n");
    expect(*client, {"ZSCORE", "z", "b"}, ",2\r\n");
    expect(*client, {"HSET", "resp3-hash", "field", "value"}, ":1\r\n");
    expect(*client, {"HGETALL", "resp3-hash"},
           "%1\r\n$5\r\nfield\r\n$5\r\nvalue\r\n");

    const auto hello2 = reply_of(*client, {"HELLO", "2"});
    assert(hello2.starts_with("*14\r\n"));
    assert(hello2.find("$5\r\nproto\r\n:2\r\n") != std::string::npos);
    expect(*client, {"GET", "missing-resp2"}, "$-1\r\n");

    // A value larger than one record: the request and the reply each split across
    // several ring records and reassemble.
    const std::string big(5000, 'z');
    expect(*client, {"SET", "big", big}, "+OK\r\n");
    expect(*client, {"STRLEN", "big"}, ":5000\r\n");
    const std::string want_big = "$5000\r\n" + big + "\r\n";
    expect(*client, {"GET", "big"}, want_big);

    // Pipelining: fire several requests before reading any reply. Priority order
    // means the server drains and answers them in order.
    for (int i = 0; i < 64; ++i) {
      client->send(std::vector<std::string_view>{"PING"});
    }
    for (int i = 0; i < 64; ++i) {
      auto r = client->read_reply();
      assert(r && *r == "+PONG\r\n");
    }
  }

  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);
  ::unlink(ring_path.c_str());
  ::unlink(sock_path.c_str());
  ::unlink(auth_path.c_str());
  ::unlink((auth_path + ".lock").c_str());
  std::puts("ring roundtrip OK");
  return rc;
}
