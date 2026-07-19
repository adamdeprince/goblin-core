// End-to-end RESP + SBE over TCP with optional ExaSock acceleration.
// Uses a loopback (or provided) address so the test can run without a live
// SmartNIC link. When launched under `exasock` against an ExaNIC IP, the
// connection is accelerated.
//
//   goblin_core_exasock_roundtrip_test <path-to-goblin-core> [HOST] [PORT]

#include "goblin/core/exasock_client.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

using goblin::core::SbeExasockClient;
using goblin::core::exasock::ExasockClient;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: exasock_roundtrip_test <goblin-core> [host] [port]\n");
    return 2;
  }
  const char* server = argv[1];
  const std::string host = argc >= 3 ? argv[2] : "127.0.0.1";
  const auto port = static_cast<std::uint16_t>(
      argc >= 4 ? std::stoul(argv[3]) : 16379U + (static_cast<unsigned>(::getpid()) % 1000U));

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      ::dup2(dn, 1);
      ::dup2(dn, 2);
    }
    const std::string port_text = std::to_string(port);
    // Prefer the ordered poll-target path so --exasock is exercised.
    ::execl(server, server, "--enable-sbe", "--exasock", host.c_str(),
            port_text.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }

  std::string error;
  std::optional<ExasockClient> resp;
  for (int i = 0; i < 200; ++i) {
    resp = ExasockClient::open(host, port, std::chrono::milliseconds(200), {},
                               &error);
    if (resp) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(resp && "could not connect RESP client");
  assert(resp->command({"PING"}) == "+PONG\r\n");
  assert(resp->command({"SET", "exasock-k", "v1"}) == "+OK\r\n");
  assert(resp->command({"GET", "exasock-k"}) == "$2\r\nv1\r\n");

  auto sbe = SbeExasockClient::open(host, port, std::chrono::seconds(2));
  assert(sbe);
  assert(sbe->ping());
  assert(sbe->set("exasock-sbe", "hello").ok);
  assert(sbe->get("exasock-sbe") == "hello");

  // INFO reports compile-time support.
  const auto info = resp->command({"INFO"});
  assert(info && info->find("exasock_support:1") != std::string::npos);

  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);
  std::puts("exasock roundtrip OK");
  return 0;
}
