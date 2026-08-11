#include "goblin/core/ring_client.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

volatile std::sig_atomic_t child_pids[2]{-1, -1};

void abort_cleanup(int signal) {
  for (const auto pid : child_pids) {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
    }
  }
  _exit(128 + signal);
}

std::uint16_t reserve_tcp_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

int connect_tcp(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int wait_for_server(std::uint16_t port) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = connect_tcp(port); fd >= 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

void send_command(int fd, std::initializer_list<std::string_view> args) {
  const std::string wire = encode_command(
      std::span<const std::string_view>(args.begin(), args.size()));
  std::size_t offset = 0;
  while (offset < wire.size()) {
    const ssize_t sent =
        ::send(fd, wire.data() + offset, wire.size() - offset, 0);
    assert(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

std::string read_reply(int fd, std::string& pending) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
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
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char buffer[4096];
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

void expect_no_reply(int fd, const std::string& pending) {
  assert(pending.empty());
  pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
  assert(::poll(&event, 1, 100) == 0);
}

std::string message(std::string_view channel, std::string_view payload,
                    bool push = false) {
  return std::string(push ? ">3\r\n" : "*3\r\n") +
         "$7\r\nmessage\r\n$" + std::to_string(channel.size()) + "\r\n" +
         std::string(channel) + "\r\n$" + std::to_string(payload.size()) +
         "\r\n" + std::string(payload) + "\r\n";
}

std::string pmessage(std::string_view pattern, std::string_view channel,
                     std::string_view payload) {
  return "*4\r\n$8\r\npmessage\r\n$" + std::to_string(pattern.size()) +
         "\r\n" + std::string(pattern) + "\r\n$" +
         std::to_string(channel.size()) + "\r\n" + std::string(channel) +
         "\r\n$" + std::to_string(payload.size()) + "\r\n" +
         std::string(payload) + "\r\n";
}

struct Child {
  pid_t pid{-1};

  explicit Child(pid_t child) : pid(child) {}
  ~Child() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
    }
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
};

Child spawn(const char* binary, const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(binary));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(binary, argv.data());
    _exit(127);
  }
  for (auto& tracked : child_pids) {
    if (tracked <= 0) {
      tracked = pid;
      break;
    }
  }
  return Child(pid);
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGABRT, abort_cleanup);
  if (argc != 3) {
    std::cerr << "usage: bluefield_socket_test <goblin-core> "
                 "<goblin-core-bluefield>\n";
    return 2;
  }

  const auto upstream_port = reserve_tcp_port();
  const auto edge_port = reserve_tcp_port();
  auto upstream = spawn(argv[1], {"--enable-sbe", "--port",
                                  std::to_string(upstream_port)});
  int probe = wait_for_server(upstream_port);
  assert(probe >= 0 && "upstream failed to start");
  ::close(probe);

  auto edge = spawn(argv[2], {"--listen", "127.0.0.1",
                              std::to_string(edge_port), "--upstream",
                              "127.0.0.1", std::to_string(upstream_port),
                              "--edge-id", "424242"});
  probe = wait_for_server(edge_port);
  assert(probe >= 0 && "BlueField edge failed to start");
  ::close(probe);

  const int ordinary = connect_tcp(edge_port);
  assert(ordinary >= 0);
  std::string ordinary_pending;
  send_command(ordinary, {"SET", "bluefield-key", "forwarded"});
  assert(read_reply(ordinary, ordinary_pending) == "+OK\r\n");
  send_command(ordinary, {"GET", "bluefield-key"});
  assert(read_reply(ordinary, ordinary_pending) == "$9\r\nforwarded\r\n");

  // The per-client upstream session preserves transactions.
  send_command(ordinary, {"MULTI"});
  assert(read_reply(ordinary, ordinary_pending) == "+OK\r\n");
  send_command(ordinary, {"SET", "transaction-key", "yes"});
  assert(read_reply(ordinary, ordinary_pending) == "+QUEUED\r\n");
  send_command(ordinary, {"EXEC"});
  assert(read_reply(ordinary, ordinary_pending) == "*1\r\n+OK\r\n");

  const int host_subscriber = connect_tcp(upstream_port);
  const int host_publisher = connect_tcp(upstream_port);
  const int edge_a = connect_tcp(edge_port);
  const int edge_b = connect_tcp(edge_port);
  const int edge_publisher = connect_tcp(edge_port);
  assert(host_subscriber >= 0 && host_publisher >= 0 && edge_a >= 0 &&
         edge_b >= 0 && edge_publisher >= 0);
  std::string host_sub_pending;
  std::string host_pub_pending;
  std::string edge_a_pending;
  std::string edge_b_pending;
  std::string edge_pub_pending;
  constexpr std::string_view channel = "prices:bluefield";

  send_command(host_subscriber, {"SUBSCRIBE", channel});
  assert(read_reply(host_subscriber, host_sub_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$16\r\nprices:bluefield\r\n:1\r\n");
  send_command(edge_a, {"SUBSCRIBE", channel});
  assert(read_reply(edge_a, edge_a_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$16\r\nprices:bluefield\r\n:1\r\n");
  send_command(edge_b, {"SUBSCRIBE", channel});
  assert(read_reply(edge_b, edge_b_pending) ==
         "*3\r\n$9\r\nsubscribe\r\n$16\r\nprices:bluefield\r\n:1\r\n");
  send_command(host_publisher, {"PUBSUB", "NUMSUB", channel});
  assert(read_reply(host_publisher, host_pub_pending) ==
         "*2\r\n$16\r\nprices:bluefield\r\n:3\r\n");

  // A host publication crosses the DPU boundary once and is fanned out locally.
  send_command(host_publisher, {"PUBLISH", channel, "from-host"});
  assert(read_reply(host_publisher, host_pub_pending) == ":3\r\n");
  assert(read_reply(host_subscriber, host_sub_pending) ==
         message(channel, "from-host"));
  assert(read_reply(edge_a, edge_a_pending) == message(channel, "from-host"));
  assert(read_reply(edge_b, edge_b_pending) == message(channel, "from-host"));

  // A DPU publication fans out locally first, reaches host subscribers, and is
  // not echoed back through the aggregate subscription.
  send_command(edge_publisher, {"PUBLISH", channel, "from-edge"});
  assert(read_reply(edge_a, edge_a_pending) == message(channel, "from-edge"));
  assert(read_reply(edge_b, edge_b_pending) == message(channel, "from-edge"));
  assert(read_reply(host_subscriber, host_sub_pending) ==
         message(channel, "from-edge"));
  assert(read_reply(edge_publisher, edge_pub_pending) == ":3\r\n");
  expect_no_reply(edge_a, edge_a_pending);
  expect_no_reply(edge_b, edge_b_pending);

  // The aggregate upstream subscription remains until the last local subscriber.
  send_command(edge_a, {"UNSUBSCRIBE", channel});
  (void)read_reply(edge_a, edge_a_pending);
  send_command(host_publisher, {"PUBLISH", channel, "one-left"});
  assert(read_reply(host_publisher, host_pub_pending) == ":2\r\n");
  assert(read_reply(host_subscriber, host_sub_pending) ==
         message(channel, "one-left"));
  assert(read_reply(edge_b, edge_b_pending) == message(channel, "one-left"));
  expect_no_reply(edge_a, edge_a_pending);

  // Pattern subscriptions are weighted independently but still share the one
  // aggregate host-to-edge publication.
  constexpr std::string_view pattern = "quotes:*";
  constexpr std::string_view pattern_channel = "quotes:bluefield";
  send_command(edge_a, {"PSUBSCRIBE", pattern});
  (void)read_reply(edge_a, edge_a_pending);
  send_command(edge_b, {"PSUBSCRIBE", pattern});
  (void)read_reply(edge_b, edge_b_pending);
  send_command(host_publisher,
               {"PUBLISH", pattern_channel, "pattern-from-host"});
  assert(read_reply(host_publisher, host_pub_pending) == ":2\r\n");
  assert(read_reply(edge_a, edge_a_pending) ==
         pmessage(pattern, pattern_channel, "pattern-from-host"));
  assert(read_reply(edge_b, edge_b_pending) ==
         pmessage(pattern, pattern_channel, "pattern-from-host"));
  send_command(edge_publisher,
               {"PUBLISH", pattern_channel, "pattern-from-edge"});
  assert(read_reply(edge_a, edge_a_pending) ==
         pmessage(pattern, pattern_channel, "pattern-from-edge"));
  assert(read_reply(edge_b, edge_b_pending) ==
         pmessage(pattern, pattern_channel, "pattern-from-edge"));
  assert(read_reply(edge_publisher, edge_pub_pending) == ":2\r\n");
  expect_no_reply(edge_a, edge_a_pending);
  expect_no_reply(edge_b, edge_b_pending);

  ::close(edge_a);
  ::close(edge_b);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // RESP3 is terminated at the edge; ordinary upstream replies are kept in the
  // same protocol version, and self-publication keeps push-before-reply order.
  const int resp3 = connect_tcp(edge_port);
  assert(resp3 >= 0);
  std::string resp3_pending;
  send_command(resp3, {"HELLO", "3"});
  const auto hello = read_reply(resp3, resp3_pending);
  assert(!hello.empty() && hello.front() == '%');
  send_command(resp3, {"GET", "missing-bluefield-key"});
  assert(read_reply(resp3, resp3_pending) == "_\r\n");
  send_command(resp3, {"SUBSCRIBE", channel});
  const auto subscribe = read_reply(resp3, resp3_pending);
  assert(!subscribe.empty() && subscribe.front() == '>');
  send_command(resp3, {"PUBLISH", channel, "self"});
  assert(read_reply(resp3, resp3_pending) == message(channel, "self", true));
  assert(read_reply(host_subscriber, host_sub_pending) == message(channel, "self"));
  assert(read_reply(resp3, resp3_pending) == ":2\r\n");
  expect_no_reply(resp3, resp3_pending);

  ::close(resp3);
  ::close(edge_publisher);
  ::close(host_publisher);
  ::close(host_subscriber);
  ::close(ordinary);
  std::cout << "BlueField RESP edge forwarding and local Pub/Sub OK\n";
  return 0;
}
