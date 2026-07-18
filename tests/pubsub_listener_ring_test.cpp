#include "goblin/core/ring_client.hpp"

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

int connect_socket(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int wait_for_socket(const std::string& path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = connect_socket(path); fd >= 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

void send_command(int fd, std::initializer_list<std::string_view> args) {
  const std::string wire = encode_command(
      std::span<const std::string_view>(args.begin(), args.size()));
  std::size_t offset = 0;
  while (offset < wire.size()) {
    const ssize_t wrote = ::send(fd, wire.data() + offset, wire.size() - offset, 0);
    assert(wrote > 0);
    offset += static_cast<std::size_t>(wrote);
  }
}

std::string read_reply(int fd, std::string& pending,
                       int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (const auto end = reply_end(pending)) {
      std::string reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&event, 1, static_cast<int>(remaining.count()));
    assert(ready > 0 && (event.revents & POLLIN) != 0);
    char buffer[4096];
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(got > 0);
    pending.append(buffer, static_cast<std::size_t>(got));
  }
}

struct Child {
  pid_t pid{-1};
  ~Child() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: pubsub_listener_ring_test <goblin-core>\n";
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  const std::string upstream_socket = "/tmp/goblin-relay-up-" + suffix + ".sock";
  const std::string downstream_socket =
      "/tmp/goblin-relay-down-" + suffix + ".sock";
  const std::string upstream_ring = "/tmp/goblin-relay-" + suffix + ".ring";
  ::unlink(upstream_socket.c_str());
  ::unlink(downstream_socket.c_str());
  ::unlink(upstream_ring.c_str());

  Child upstream{.pid = ::fork()};
  assert(upstream.pid >= 0);
  if (upstream.pid == 0) {
    ::execl(argv[1], argv[1], "--unixsocket", upstream_socket.c_str(), "--ring",
            upstream_ring.c_str(), "64kb", static_cast<char*>(nullptr));
    _exit(127);
  }

  const int publisher = wait_for_socket(upstream_socket);
  assert(publisher >= 0 && "upstream server failed to start");

  Child downstream{.pid = ::fork()};
  assert(downstream.pid >= 0);
  if (downstream.pid == 0) {
    ::execl(argv[1], argv[1], "--unixsocket", downstream_socket.c_str(),
            "--pubsub-listener-ring", upstream_ring.c_str(),
            "--pubsub-listener-pattern", "orders:*", static_cast<char*>(nullptr));
    _exit(127);
  }

  const int subscriber = wait_for_socket(downstream_socket);
  assert(subscriber >= 0 && "downstream relay server failed to start");

  std::string publisher_pending;
  std::string subscriber_pending;
  send_command(subscriber, {"SUBSCRIBE", "orders:filled"});
  assert(read_reply(subscriber, subscriber_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$13\r\norders:filled\r\n:1\r\n");

  send_command(publisher, {"PUBLISH", "ignored", "no relay"});
  assert(read_reply(publisher, publisher_pending) == ":0\r\n");
  pollfd quiet{.fd = subscriber, .events = POLLIN, .revents = 0};
  assert(::poll(&quiet, 1, 100) == 0);

  send_command(publisher, {"PUBLISH", "orders:filled", "trade-42"});
  assert(read_reply(publisher, publisher_pending) == ":1\r\n");
  assert(read_reply(subscriber, subscriber_pending) ==
         "*3\r\n$7\r\nmessage\r\n$13\r\norders:filled\r\n$8\r\ntrade-42\r\n");

  ::close(subscriber);
  ::close(publisher);
  ::unlink(upstream_socket.c_str());
  ::unlink(downstream_socket.c_str());
  ::unlink(upstream_ring.c_str());
  std::cout << "Pub/Sub listener ring relay OK\n";
  return 0;
}
