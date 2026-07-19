#include "goblin/core/ring_client.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
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
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

int connect_ipv4(std::uint16_t port) {
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

int connect_ipv6(std::uint16_t port) {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(port);
  address.sin6_addr = in6addr_loopback;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  assert(path.size() < sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
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

std::uint16_t reserve_port(int family) {
  const int fd = ::socket(family, SOCK_STREAM, 0);
  assert(fd >= 0);
  if (family == AF_INET) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) ==
           0);
    const auto port = ntohs(address.sin_port);
    ::close(fd);
    return port;
  }

  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = 0;
  address.sin6_addr = in6addr_loopback;
  assert(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) ==
         0);
  const auto port = ntohs(address.sin6_port);
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: socket_listeners_test <goblin-core>\n";
    return 2;
  }

  const auto tcp_a = reserve_port(AF_INET);
  const auto tcp_b = reserve_port(AF_INET);
  const auto tcp_v6 = reserve_port(AF_INET6);
  const auto tcp_uds_only = reserve_port(AF_INET);
  const std::string suffix = std::to_string(::getpid());
  const std::string uds_a = "/tmp/goblin-listener-a-" + suffix + ".sock";
  const std::string uds_b = "/tmp/goblin-listener-b-" + suffix + ".sock";
  const std::string uds_legacy =
      "/tmp/goblin-listener-legacy-" + suffix + ".sock";
  const std::string uds_only =
      "/tmp/goblin-listener-only-" + suffix + ".sock";
  (void)::unlink(uds_a.c_str());
  (void)::unlink(uds_b.c_str());
  (void)::unlink(uds_legacy.c_str());
  (void)::unlink(uds_only.c_str());

  {
    auto server = spawn_server(
        argv[1],
        {"--listen", ":" + std::to_string(tcp_a),
         "--listen", "127.0.0.1:" + std::to_string(tcp_b),
         "--tcp-listen", "[::1]:" + std::to_string(tcp_v6), "--uds-listen",
         uds_a, "--uds-listen", uds_b, "--unixsocket", uds_legacy});

    std::vector<int> clients;
    clients.push_back(
        wait_for_connection([&] { return connect_ipv4(tcp_a); }));
    clients.push_back(
        wait_for_connection([&] { return connect_ipv4(tcp_b); }));
    clients.push_back(
        wait_for_connection([&] { return connect_ipv6(tcp_v6); }));
    clients.push_back(
        wait_for_connection([&] { return connect_ipv4(tcp_v6); }));
    clients.push_back(wait_for_connection([&] { return connect_uds(uds_a); }));
    clients.push_back(wait_for_connection([&] { return connect_uds(uds_b); }));
    clients.push_back(
        wait_for_connection([&] { return connect_uds(uds_legacy); }));
    for (const int fd : clients) {
      assert(fd >= 0 && "server did not bind every configured listener");
    }

    std::vector<std::string> pending(clients.size());
    for (std::size_t i = 0; i < clients.size(); ++i) {
      send_command(clients[i], {"PING"});
      assert(read_reply(clients[i], pending[i]) == "+PONG\r\n");
    }

    send_command(clients.front(), {"SET", "multi-listener-key", "shared"});
    assert(read_reply(clients.front(), pending.front()) == "+OK\r\n");
    for (std::size_t i = 0; i < clients.size(); ++i) {
      send_command(clients[i], {"GET", "multi-listener-key"});
      assert(read_reply(clients[i], pending[i]) == "$6\r\nshared\r\n");
      ::close(clients[i]);
    }
  }

  {
    auto server = spawn_server(
        argv[1], {"--uds-listen", uds_only, "--port",
                  std::to_string(tcp_uds_only)});
    const int uds = wait_for_connection([&] { return connect_uds(uds_only); });
    const int local =
        wait_for_connection([&] { return connect_ipv4(tcp_uds_only); });
    assert(uds >= 0 && local >= 0 &&
           "UDS configuration did not retain its localhost listener");
    std::string uds_pending;
    std::string local_pending;
    send_command(uds, {"PING"});
    send_command(local, {"PING"});
    assert(read_reply(uds, uds_pending) == "+PONG\r\n");
    assert(read_reply(local, local_pending) == "+PONG\r\n");
    ::close(uds);
    ::close(local);
  }

  (void)::unlink(uds_a.c_str());
  (void)::unlink(uds_b.c_str());
  (void)::unlink(uds_legacy.c_str());
  (void)::unlink(uds_only.c_str());
  std::cout << "simultaneous listeners and mandatory localhost OK\n";
  return 0;
}
