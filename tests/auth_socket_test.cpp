#include "goblin/core/auth.hpp"
#include "goblin/core/ring_client.hpp"
#include "socket_test_utils.hpp"

#undef NDEBUG
#include <cassert>
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
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

int connect_socket(const std::string& path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      return fd;
    }
    (void)::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
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
    char buffer[8192];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: auth_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  const std::string socket_path = "/tmp/goblin-auth-" + suffix + ".sock";
  const std::string auth_path = "/tmp/goblin-auth-" + suffix + ".conf";
  const std::string lock_path = auth_path + ".lock";
  (void)::unlink(socket_path.c_str());
  (void)::unlink(auth_path.c_str());
  (void)::unlink(lock_path.c_str());
  assert(goblin::core::upsert_auth_user(auth_path, "default", "secret") ==
         goblin::core::AuthUserUpdate::added);
  assert(goblin::core::upsert_auth_user(auth_path, "worker", "worker-secret") ==
         goblin::core::AuthUserUpdate::added);
  const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
  assert(tcp_port != 0);
  const std::string tcp_port_text = std::to_string(tcp_port);

  const pid_t server = ::fork();
  assert(server >= 0);
  if (server == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)::dup2(devnull, STDOUT_FILENO);
      (void)::dup2(devnull, STDERR_FILENO);
    }
    ::execl(argv[1], argv[1], "--unixsocket", socket_path.c_str(),
            "--port", tcp_port_text.c_str(), "--auth-file", auth_path.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }

  const int first = connect_socket(socket_path);
  assert(first >= 0);
  std::string pending;
  send_command(first, {"PING"});
  assert(read_reply(first, pending) == "-NOAUTH Authentication required.\r\n");
  send_command(first, {"AUTH", "worker", "wrong"});
  assert(read_reply(first, pending).starts_with("-WRONGPASS "));
  send_command(first, {"AUTH", "worker", "worker-secret"});
  assert(read_reply(first, pending) == "+OK\r\n");
  send_command(first, {"PING"});
  assert(read_reply(first, pending) == "+PONG\r\n");

  const int second = connect_socket(socket_path);
  assert(second >= 0);
  std::string second_pending;
  send_command(second,
               {"HELLO", "3", "AUTH", "default", "secret", "SETNAME", "app"});
  const auto hello = read_reply(second, second_pending);
  assert(hello.starts_with("%7\r\n"));
  assert(hello.find("$5\r\nproto\r\n:3\r\n") != std::string::npos);
  send_command(second, {"CLIENT", "GETNAME"});
  assert(read_reply(second, second_pending) == "$3\r\napp\r\n");
  send_command(second, {"CLIENT", "SETINFO", "LIB-NAME", "goblin-test"});
  assert(read_reply(second, second_pending) == "+OK\r\n");
  send_command(second, {"CLIENT", "SETINFO", "LIB-VER", "1.0"});
  assert(read_reply(second, second_pending) == "+OK\r\n");
  send_command(second, {"CLIENT", "ID"});
  assert(read_reply(second, second_pending).starts_with(":"));
  send_command(second, {"SELECT", "0"});
  assert(read_reply(second, second_pending) == "+OK\r\n");
  send_command(second, {"SELECT", "1"});
  assert(read_reply(second, second_pending) == "-ERR DB index is out of range\r\n");
  send_command(second, {"COMMAND", "INFO", "HSET", "NOT-A-COMMAND"});
  const auto command_info = read_reply(second, second_pending);
  assert(command_info.starts_with("*2\r\n*10\r\n$4\r\nhset\r\n"));
  assert(command_info.find("$4\r\nhset\r\n:-4\r\n") != std::string::npos);
  assert(command_info.ends_with("_\r\n"));
  send_command(second, {"QUIT"});
  assert(read_reply(second, second_pending) == "+OK\r\n");
  char byte{};
  assert(::recv(second, &byte, 1, 0) == 0);

  const int third = connect_socket(socket_path);
  assert(third >= 0);
  std::string third_pending;
  send_command(third, {"AUTH", "secret"});
  send_command(third, {"SET", "authenticated-pipeline", "yes"});
  assert(read_reply(third, third_pending) == "+OK\r\n");
  assert(read_reply(third, third_pending) == "+OK\r\n");

  (void)::close(third);
  (void)::close(second);
  (void)::close(first);
  (void)::kill(server, SIGTERM);
  (void)::waitpid(server, nullptr, 0);
  (void)::unlink(socket_path.c_str());
  (void)::unlink(auth_path.c_str());
  (void)::unlink(lock_path.c_str());
  std::puts("authentication socket compatibility OK");
  return 0;
}
