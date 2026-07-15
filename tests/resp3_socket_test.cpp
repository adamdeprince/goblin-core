// End-to-end RESP3 negotiation over a Unix-domain socket. The client talks RESP2
// first, pipelines HELLO 3 with a following command, verifies RESP3-native reply
// shapes, then switches back to RESP2. A second connection proves negotiation is
// connection-local.

#include "goblin/core/ring_client.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

int connect_socket(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::close(fd);
  return -1;
}

void send_all(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto sent = ::send(fd, bytes.data(), bytes.size(), 0);
    assert(sent > 0);
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
}

void send_command(int fd, std::initializer_list<std::string_view> args) {
  const auto command = encode_command(
      std::span<const std::string_view>(args.begin(), args.size()));
  send_all(fd, command);
}

std::string read_reply(int fd, std::string& pending) {
  for (;;) {
    if (const auto end = reply_end(pending)) {
      std::string reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    char buffer[4096];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: resp3_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string socket_path =
      "/tmp/goblin-resp3-" + std::to_string(::getpid()) + ".sock";
  ::unlink(socket_path.c_str());

  const pid_t server = ::fork();
  assert(server >= 0);
  if (server == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    ::execl(argv[1], argv[1], "--unixsocket", socket_path.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }

  const int first = connect_socket(socket_path);
  assert(first >= 0);
  std::string pending;
  send_command(first, {"PING"});
  assert(read_reply(first, pending) == "+PONG\r\n");

  // HELLO is not restricted to the first command. The following GET is already
  // parsed in the same pipeline and must use the newly selected RESP3 encoding.
  send_command(first, {"HELLO", "3"});
  send_command(first, {"GET", "missing"});
  const auto hello3 = read_reply(first, pending);
  assert(hello3.starts_with("%7\r\n"));
  assert(hello3.find("$5\r\nproto\r\n:3\r\n") != std::string::npos);
  assert(read_reply(first, pending) == "_\r\n");

  send_command(first, {"HSET", "h", "field", "value"});
  assert(read_reply(first, pending) == ":1\r\n");
  send_command(first, {"HGETALL", "h"});
  assert(read_reply(first, pending) ==
         "%1\r\n$5\r\nfield\r\n$5\r\nvalue\r\n");

  // A newly accepted connection still starts in RESP2.
  const int second = connect_socket(socket_path);
  assert(second >= 0);
  std::string second_pending;
  send_command(second, {"GET", "missing"});
  assert(read_reply(second, second_pending) == "$-1\r\n");

  send_command(first, {"HELLO", "2"});
  const auto hello2 = read_reply(first, pending);
  assert(hello2.starts_with("*14\r\n"));
  assert(hello2.find("$5\r\nproto\r\n:2\r\n") != std::string::npos);
  send_command(first, {"GET", "missing"});
  assert(read_reply(first, pending) == "$-1\r\n");

  ::close(second);
  ::close(first);
  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);
  ::unlink(socket_path.c_str());
  std::puts("RESP3 socket negotiation OK");
  return 0;
}
