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
#include <vector>

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
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        0) {
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

void send_command(int fd, std::span<const std::string_view> args) {
  send_all(fd, encode_command(args));
}

void send_command(int fd, std::initializer_list<std::string_view> args) {
  send_command(fd,
               std::span<const std::string_view>(args.begin(), args.size()));
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

void expect(int fd, std::string& pending,
            std::initializer_list<std::string_view> command,
            std::string_view reply) {
  send_command(fd, command);
  assert(read_reply(fd, pending) == reply);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: transaction_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string socket_path =
      "/tmp/goblin-transaction-" + std::to_string(::getpid()) + ".sock";
  (void)::unlink(socket_path.c_str());
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
            "--port", tcp_port_text.c_str(), "--transaction-buffer-bytes", "1",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  const int first = connect_socket(socket_path);
  const int second = connect_socket(socket_path);
  assert(first >= 0 && second >= 0);
  std::string pending;
  std::string second_pending;

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"SET", "counter", "1"}, "+QUEUED\r\n");
  expect(first, pending, {"INCR", "counter"}, "+QUEUED\r\n");
  expect(first, pending, {"GET", "counter"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"},
         "*3\r\n+OK\r\n:2\r\n$1\r\n2\r\n");

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"MULTI"},
         "-ERR MULTI calls can not be nested\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*1\r\n+PONG\r\n");

  expect(first, pending, {"SET", "typed", "string"}, "+OK\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"HSET", "typed", "field", "value"},
         "+QUEUED\r\n");
  expect(first, pending, {"SET", "after-runtime-error", "yes"},
         "+QUEUED\r\n");
  send_command(first, {"EXEC"});
  const auto runtime_results = read_reply(first, pending);
  assert(runtime_results.starts_with("*2\r\n-WRONGTYPE "));
  assert(runtime_results.ends_with("+OK\r\n"));
  expect(first, pending, {"GET", "after-runtime-error"},
         "$3\r\nyes\r\n");

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"SET", "wrong-arity"},
         "-ERR wrong number of arguments for 'set' command\r\n");
  expect(first, pending, {"SET", "must-not-run", "x"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"},
         "-EXECABORT Transaction discarded because of previous errors.\r\n");
  expect(first, pending, {"GET", "must-not-run"}, "$-1\r\n");

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  send_command(first, {"NOT-A-COMMAND"});
  assert(read_reply(first, pending).starts_with("-ERR unknown command "));
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"},
         "-EXECABORT Transaction discarded because of previous errors.\r\n");

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"SET", "discarded", "x"}, "+QUEUED\r\n");
  expect(first, pending, {"DISCARD"}, "+OK\r\n");
  expect(first, pending, {"GET", "discarded"}, "$-1\r\n");
  expect(first, pending, {"EXEC"}, "-ERR EXEC without MULTI\r\n");
  expect(first, pending, {"DISCARD"}, "-ERR DISCARD without MULTI\r\n");

  // Same-value writes still modify the watched key; a nonexistent DEL does not.
  expect(first, pending, {"SET", "watched", "same"}, "+OK\r\n");
  expect(first, pending, {"WATCH", "watched"}, "+OK\r\n");
  expect(second, second_pending, {"SET", "watched", "same"}, "+OK\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"GET", "watched"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*-1\r\n");

  expect(first, pending, {"WATCH", "absent"}, "+OK\r\n");
  expect(second, second_pending, {"DEL", "absent"}, ":0\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*1\r\n+PONG\r\n");

  expect(first, pending, {"WATCH", "watched"}, "+OK\r\n");
  expect(second, second_pending, {"SET", "unrelated", "value"}, "+OK\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*1\r\n+PONG\r\n");

  // A conditional write that does not apply leaves WATCH clean.
  expect(first, pending, {"WATCH", "watched"}, "+OK\r\n");
  expect(second, second_pending, {"SET", "watched", "ignored", "NX"},
         "$-1\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*1\r\n+PONG\r\n");

  // HSET returns zero for an existing field, but assigning it is still a write.
  expect(first, pending, {"HSET", "hash-watch", "field", "value"},
         ":1\r\n");
  expect(first, pending, {"WATCH", "hash-watch"}, "+OK\r\n");
  expect(second, second_pending,
         {"HSET", "hash-watch", "field", "value"}, ":0\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*-1\r\n");

  // Expiration is a modification even when no client explicitly deletes the key.
  expect(first, pending, {"SET", "expires-while-watched", "v", "PX", "20"},
         "+OK\r\n");
  expect(first, pending, {"WATCH", "expires-while-watched"}, "+OK\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*-1\r\n");

  expect(first, pending, {"WATCH", "watched"}, "+OK\r\n");
  expect(first, pending, {"UNWATCH"}, "+OK\r\n");
  expect(second, second_pending, {"SET", "watched", "changed"}, "+OK\r\n");
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"UNWATCH"}, "+QUEUED\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*2\r\n+OK\r\n+PONG\r\n");

  expect(first, pending, {"MULTI"}, "+OK\r\n");
  expect(first, pending, {"WATCH", "watched"},
         "-ERR WATCH inside MULTI is not allowed\r\n");
  expect(first, pending, {"PING"}, "+QUEUED\r\n");
  expect(first, pending, {"EXEC"}, "*1\r\n+PONG\r\n");

  // One byte on the CLI rounds to one native page. A command larger than that
  // is discarded, later commands are drained, and EXEC resets the connection.
  const long native_page = ::sysconf(_SC_PAGESIZE);
  assert(native_page > 0);
  const std::string oversized(static_cast<std::size_t>(native_page) + 128, 'x');
  const std::vector<std::string_view> echo{"ECHO", oversized};
  expect(first, pending, {"MULTI"}, "+OK\r\n");
  send_command(first, echo);
  const auto overflow = read_reply(first, pending);
  assert(overflow.starts_with("-ERR transaction exceeds the configured "));
  expect(first, pending, {"PING"}, overflow);
  send_command(first, {"EXEC"});
  const auto abort = read_reply(first, pending);
  assert(abort.starts_with(
      "-EXECABORT Transaction discarded because its "));
  assert(abort.ends_with("-byte buffer limit was exceeded.\r\n"));
  expect(first, pending, {"PING"}, "+PONG\r\n");

  const int resp3_client = connect_socket(socket_path);
  assert(resp3_client >= 0);
  std::string resp3_pending;
  send_command(resp3_client, {"HELLO", "3"});
  assert(read_reply(resp3_client, resp3_pending).starts_with("%7\r\n"));
  expect(resp3_client, resp3_pending, {"WATCH", "watched"}, "+OK\r\n");
  expect(second, second_pending, {"SET", "watched", "resp3"}, "+OK\r\n");
  expect(resp3_client, resp3_pending, {"MULTI"}, "+OK\r\n");
  expect(resp3_client, resp3_pending, {"PING"}, "+QUEUED\r\n");
  expect(resp3_client, resp3_pending, {"EXEC"}, "_\r\n");
  (void)::close(resp3_client);

  expect(first, pending, {"COMMAND", "INFO", "MULTI", "WATCH"},
         "*2\r\n*10\r\n$5\r\nmulti\r\n:1\r\n*1\r\n$4\r\nfast\r\n:0\r\n:0\r\n:0\r\n*1\r\n$12\r\n@transaction\r\n*0\r\n*0\r\n*0\r\n"
         "*10\r\n$5\r\nwatch\r\n:-2\r\n*1\r\n$4\r\nfast\r\n:1\r\n:-1\r\n:1\r\n*1\r\n$12\r\n@transaction\r\n*0\r\n*0\r\n*0\r\n");

  (void)::close(second);
  (void)::close(first);
  (void)::kill(server, SIGTERM);
  (void)::waitpid(server, nullptr, 0);
  (void)::unlink(socket_path.c_str());
  std::puts("transaction socket compatibility OK");
  return 0;
}
