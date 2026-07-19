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
#include <sys/un.h>
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

int connect_uds(const std::string& path) {
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

int connect_tcp(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
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

template <class Connect>
int wait_for_connection(Connect&& connect) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = std::invoke(connect); fd >= 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
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

Child spawn_server(const char* binary, const std::vector<std::string>& args) {
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
  return Child(pid);
}

template <class PublisherConnect>
void run_case(const char* binary, std::string_view name,
              const std::vector<std::string>& upstream_args,
              const std::vector<std::string>& listener_args,
              PublisherConnect&& publisher_connect) {
  const std::string suffix = std::to_string(::getpid()) + '-' + std::string(name);
  const std::string downstream_socket =
      "/tmp/goblin-relay-socket-down-" + suffix + ".sock";
  (void)::unlink(downstream_socket.c_str());

  {
    std::vector<std::string> trusted_upstream_args{"--enable-sbe"};
    trusted_upstream_args.insert(trusted_upstream_args.end(),
                                 upstream_args.begin(), upstream_args.end());
    auto upstream = spawn_server(binary, trusted_upstream_args);
    const int publisher = wait_for_connection(publisher_connect);
    assert(publisher >= 0 && "upstream server failed to start");

    const auto downstream_port = reserve_tcp_port();
    std::vector<std::string> downstream_args = {
        "--enable-sbe", "--unixsocket", downstream_socket, "--port",
        std::to_string(downstream_port)};
    downstream_args.insert(downstream_args.end(), listener_args.begin(),
                           listener_args.end());
    auto downstream = spawn_server(binary, downstream_args);
    const int subscriber =
        wait_for_connection([&] { return connect_uds(downstream_socket); });
    assert(subscriber >= 0 && "downstream relay server failed to start");

    std::string publisher_pending;
    std::string subscriber_pending;
    send_command(subscriber, {"SUBSCRIBE", "events:filled"});
    assert(read_reply(subscriber, subscriber_pending) ==
           "*3\r\n$9\r\nsubscribe\r\n$13\r\nevents:filled\r\n:1\r\n");

    send_command(publisher, {"PUBLISH", "events:filled", name});
    assert(read_reply(publisher, publisher_pending) == ":1\r\n");
    const std::string expected =
        "*3\r\n$7\r\nmessage\r\n$13\r\nevents:filled\r\n$" +
        std::to_string(name.size()) + "\r\n" + std::string(name) + "\r\n";
    assert(read_reply(subscriber, subscriber_pending) == expected);

    ::close(subscriber);
    ::close(publisher);
  }
  (void)::unlink(downstream_socket.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: pubsub_listener_socket_test <goblin-core>\n";
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  const std::string upstream_socket =
      "/tmp/goblin-relay-socket-up-" + suffix + ".sock";
  (void)::unlink(upstream_socket.c_str());
  const auto uds_tcp_port = reserve_tcp_port();
  run_case(argv[1], "uds",
           {"--unixsocket", upstream_socket, "--port",
            std::to_string(uds_tcp_port)},
           {"--pubsub-listener-uds", upstream_socket},
           [&] { return connect_uds(upstream_socket); });
  (void)::unlink(upstream_socket.c_str());

  const auto port = reserve_tcp_port();
  const auto port_text = std::to_string(port);
  run_case(argv[1], "tcp", {"--bind", "127.0.0.1", "--port", port_text},
           {"--pubsub-listener-tcp", "127.0.0.1", port_text},
           [&] { return connect_tcp(port); });

  std::cout << "Pub/Sub listener UDS and TCP relays OK\n";
  return 0;
}
