// End-to-end Pub/Sub over a Unix socket: RESP2 restrictions, RESP3 pushes,
// literal and glob subscriptions, duplicate handling, disconnect cleanup, and
// the page-backed unsolicited-output limit.

#include "goblin/core/ring_client.hpp"
#include "socket_test_utils.hpp"

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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
  send_all(fd, encode_command(
                   std::span<const std::string_view>(args.begin(), args.size())));
}

std::string read_reply(int fd, std::string& pending) {
  for (;;) {
    if (const auto end = reply_end(pending)) {
      std::string reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    char buffer[8192];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: pubsub_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string socket_path =
      "/tmp/goblin-pubsub-" + std::to_string(::getpid()) + ".sock";
  ::unlink(socket_path.c_str());
  const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
  assert(tcp_port != 0);
  const std::string tcp_port_text = std::to_string(tcp_port);

  const pid_t server = ::fork();
  assert(server >= 0);
  if (server == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
    }
    ::execl(argv[1], argv[1], "--unixsocket", socket_path.c_str(),
            "--port", tcp_port_text.c_str(),
            "--unsolicited-output-buffer-bytes", "1",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  const int publisher = connect_socket(socket_path);
  const int resp2 = connect_socket(socket_path);
  const int resp3 = connect_socket(socket_path);
  assert(publisher >= 0 && resp2 >= 0 && resp3 >= 0);
  std::string publisher_pending;
  std::string resp2_pending;
  std::string resp3_pending;

  send_command(resp2, {"SUBSCRIBE", "alpha", "alpha"});
  assert(read_reply(resp2, resp2_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$5\r\nalpha\r\n:1\r\n");
  assert(read_reply(resp2, resp2_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$5\r\nalpha\r\n:1\r\n");

  send_command(resp2, {"GET", "forbidden"});
  assert(read_reply(resp2, resp2_pending) ==
         "-ERR Can't execute this command while subscribed to a channel\r\n");
  send_command(resp2, {"PING", "token"});
  assert(read_reply(resp2, resp2_pending) ==
         "*2\r\n$4\r\npong\r\n$5\r\ntoken\r\n");

  send_command(resp3, {"HELLO", "3"});
  assert(read_reply(resp3, resp3_pending).starts_with("%7\r\n"));
  send_command(resp3, {"SUBSCRIBE", "alpha"});
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$9\r\nsubscribe\r\n$5\r\nalpha\r\n:1\r\n");
  send_command(resp3, {"PSUBSCRIBE", "a[0-z]*"});
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$10\r\npsubscribe\r\n$7\r\na[0-z]*\r\n:2\r\n");
  send_command(resp3, {"SET", "ordinary", "works"});
  assert(read_reply(resp3, resp3_pending) == "+OK\r\n");

  // PUBLISH is script-safe. Pushes caused by the script precede its integer
  // reply on the same RESP3 connection.
  send_command(resp3,
               {"EVAL", "return redis.call('PUBLISH','alpha','scripted')", "0"});
  assert(read_reply(resp2, resp2_pending) ==
         "*3\r\n$7\r\nmessage\r\n$5\r\nalpha\r\n$8\r\nscripted\r\n");
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$7\r\nmessage\r\n$5\r\nalpha\r\n$8\r\nscripted\r\n");
  assert(read_reply(resp3, resp3_pending) ==
         ">4\r\n$8\r\npmessage\r\n$7\r\na[0-z]*\r\n$5\r\nalpha\r\n$8\r\nscripted\r\n");
  assert(read_reply(resp3, resp3_pending) == ":3\r\n");

  send_command(publisher, {"PUBLISH", "alpha", "payload"});
  assert(read_reply(publisher, publisher_pending) == ":3\r\n");
  assert(read_reply(resp2, resp2_pending) ==
         "*3\r\n$7\r\nmessage\r\n$5\r\nalpha\r\n$7\r\npayload\r\n");
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$7\r\nmessage\r\n$5\r\nalpha\r\n$7\r\npayload\r\n");
  assert(read_reply(resp3, resp3_pending) ==
         ">4\r\n$8\r\npmessage\r\n$7\r\na[0-z]*\r\n$5\r\nalpha\r\n$7\r\npayload\r\n");

  send_command(publisher, {"PUBSUB", "NUMSUB", "alpha", "missing"});
  assert(read_reply(publisher, publisher_pending) ==
         "*4\r\n$5\r\nalpha\r\n:2\r\n$7\r\nmissing\r\n:0\r\n");
  send_command(publisher, {"PUBSUB", "NUMPAT"});
  assert(read_reply(publisher, publisher_pending) == ":1\r\n");

  ::close(resp2);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  send_command(publisher, {"PUBLISH", "alpha", "again"});
  assert(read_reply(publisher, publisher_pending) == ":2\r\n");
  (void)read_reply(resp3, resp3_pending);
  (void)read_reply(resp3, resp3_pending);

  send_command(resp3, {"PUNSUBSCRIBE"});
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$12\r\npunsubscribe\r\n$7\r\na[0-z]*\r\n:1\r\n");
  send_command(resp3, {"UNSUBSCRIBE"});
  assert(read_reply(resp3, resp3_pending) ==
         ">3\r\n$11\r\nunsubscribe\r\n$5\r\nalpha\r\n:0\r\n");

  // One requested byte rounds to one native page. A delivery larger than that
  // cannot be queued, so the subscriber is removed and disconnected atomically.
  const int slow = connect_socket(socket_path);
  assert(slow >= 0);
  std::string slow_pending;
  send_command(slow, {"SUBSCRIBE", "oversized"});
  (void)read_reply(slow, slow_pending);
  const std::string large_payload(32 * 1024, 'x');
  send_command(publisher, {"PUBLISH", "oversized", large_payload});
  assert(read_reply(publisher, publisher_pending) == ":0\r\n");
  timeval timeout{.tv_sec = 1, .tv_usec = 0};
  (void)::setsockopt(slow, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char byte = 0;
  assert(::recv(slow, &byte, 1, 0) == 0);

  send_command(publisher, {"PUBSUB", "NUMSUB", "oversized"});
  assert(read_reply(publisher, publisher_pending) ==
         "*2\r\n$9\r\noversized\r\n:0\r\n");

  ::close(slow);
  ::close(resp3);
  ::close(publisher);
  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);
  ::unlink(socket_path.c_str());
  std::puts("Pub/Sub socket behavior OK");
  return 0;
}
