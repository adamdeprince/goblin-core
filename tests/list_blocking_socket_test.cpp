#include "goblin/core/ring_client.hpp"

#undef NDEBUG
#include <array>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
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

std::string read_reply(int fd, std::string& pending,
                       std::chrono::milliseconds timeout =
                           std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
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
    char buffer[8192];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

void expect(int fd, std::string& pending,
            std::initializer_list<std::string_view> command,
            std::string_view expected) {
  send_command(fd, command);
  const auto actual = read_reply(fd, pending);
  if (actual != expected) {
    std::fprintf(stderr, "%.*s: expected [%.*s], got [%.*s]\n",
                 static_cast<int>(command.begin()->size()),
                 command.begin()->data(), static_cast<int>(expected.size()),
                 expected.data(), static_cast<int>(actual.size()), actual.data());
    std::abort();
  }
}

void expect_no_reply(int fd, std::chrono::milliseconds duration) {
  pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
  assert(::poll(&event, 1, static_cast<int>(duration.count())) == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: list_blocking_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string socket_path =
      "/tmp/goblin-list-blocking-" + std::to_string(::getpid()) + ".sock";
  (void)::unlink(socket_path.c_str());

  const pid_t server = ::fork();
  assert(server >= 0);
  if (server == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)::dup2(devnull, STDOUT_FILENO);
      (void)::dup2(devnull, STDERR_FILENO);
    }
    ::execl(argv[1], argv[1], "--unixsocket", socket_path.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }

  const int first = connect_socket(socket_path);
  const int second = connect_socket(socket_path);
  const int producer = connect_socket(socket_path);
  assert(first >= 0 && second >= 0 && producer >= 0);
  std::string first_pending;
  std::string second_pending;
  std::string producer_pending;

  // A pipelined command behind BLPOP remains parked, while another connection
  // can still execute and wake the blocked client.
  send_all(first, encode_command(std::array<std::string_view, 3>{
                      "BLPOP", "work", "0"}) +
                      encode_command(
                          std::array<std::string_view, 1>{"PING"}));
  expect_no_reply(first, std::chrono::milliseconds(30));
  expect(producer, producer_pending, {"PING"}, "+PONG\r\n");
  expect(producer, producer_pending, {"RPUSH", "work", "one"}, ":1\r\n");
  assert(read_reply(first, first_pending) ==
         "*2\r\n$4\r\nwork\r\n$3\r\none\r\n");
  assert(read_reply(first, first_pending) == "+PONG\r\n");

  // Blocked clients are FIFO. One batched push wakes both in registration order.
  send_command(first, {"BLPOP", "fifo", "0"});
  expect_no_reply(first, std::chrono::milliseconds(20));
  send_command(second, {"BLPOP", "fifo", "0"});
  expect_no_reply(second, std::chrono::milliseconds(20));
  expect(producer, producer_pending, {"RPUSH", "fifo", "first", "second"},
         ":2\r\n");
  assert(read_reply(first, first_pending) ==
         "*2\r\n$4\r\nfifo\r\n$5\r\nfirst\r\n");
  assert(read_reply(second, second_pending) ==
         "*2\r\n$4\r\nfifo\r\n$6\r\nsecond\r\n");

  send_command(first, {"BRPOP", "right", "0"});
  expect_no_reply(first, std::chrono::milliseconds(20));
  expect(producer, producer_pending, {"LPUSH", "right", "a", "b"},
         ":2\r\n");
  assert(read_reply(first, first_pending) ==
         "*2\r\n$5\r\nright\r\n$1\r\na\r\n");

  send_command(first, {"BLMOVE", "move-in", "move-out", "RIGHT", "LEFT",
                       "0"});
  expect_no_reply(first, std::chrono::milliseconds(20));
  expect(producer, producer_pending, {"RPUSH", "move-in", "moved"},
         ":1\r\n");
  assert(read_reply(first, first_pending) == "$5\r\nmoved\r\n");
  expect(producer, producer_pending, {"LRANGE", "move-out", "0", "-1"},
         "*1\r\n$5\r\nmoved\r\n");

  send_command(first,
               {"BLMPOP", "0", "2", "missing", "batch", "LEFT", "COUNT",
                "2"});
  expect_no_reply(first, std::chrono::milliseconds(20));
  expect(producer, producer_pending, {"RPUSH", "batch", "a", "b", "c"},
         ":3\r\n");
  assert(read_reply(first, first_pending) ==
         "*2\r\n$5\r\nbatch\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n");

  // Qualified command families park through the same lifecycle.
  send_command(first, {"GOBLIN.PMA.BLPOP", "pma-block", "0"});
  expect_no_reply(first, std::chrono::milliseconds(20));
  expect(producer, producer_pending,
         {"GOBLIN.PMA.RPUSH", "pma-block", "pma"}, ":1\r\n");
  assert(read_reply(first, first_pending) ==
         "*2\r\n$9\r\npma-block\r\n$3\r\npma\r\n");

  const auto timeout_start = std::chrono::steady_clock::now();
  send_command(first, {"BLPOP", "times-out", "0.05"});
  assert(read_reply(first, first_pending) == "*-1\r\n");
  assert(std::chrono::steady_clock::now() - timeout_start >=
         std::chrono::milliseconds(30));
  expect(first, first_pending,
         {"BLMOVE", "times-out", "move-out", "LEFT", "RIGHT", "0.01"},
         "$-1\r\n");

  expect(producer, producer_pending, {"SET", "wrong-list-type", "value"},
         "+OK\r\n");
  send_command(first, {"BLPOP", "missing", "wrong-list-type", "1"});
  assert(read_reply(first, first_pending)
             .starts_with("-WRONGTYPE Operation against a key"));
  expect(producer, producer_pending,
         {"RPUSH", "move-before-wrong-type", "preserved"}, ":1\r\n");
  send_command(first,
               {"LMOVE", "move-before-wrong-type", "wrong-list-type",
                "RIGHT", "LEFT"});
  assert(read_reply(first, first_pending)
             .starts_with("-WRONGTYPE Operation against a key"));
  expect(producer, producer_pending, {"LPOP", "move-before-wrong-type"},
         "$9\r\npreserved\r\n");
  // An unavailable source does not inspect BLMOVE's destination type.
  expect(first, first_pending,
         {"BLMOVE", "missing", "wrong-list-type", "LEFT", "RIGHT", "0.01"},
         "$-1\r\n");

  // Blocking commands inside EXEC use their non-blocking form.
  expect(first, first_pending, {"MULTI"}, "+OK\r\n");
  expect(first, first_pending, {"BLPOP", "transaction-empty", "0"},
         "+QUEUED\r\n");
  expect(first, first_pending, {"PING"}, "+QUEUED\r\n");
  expect(first, first_pending, {"EXEC"},
         "*2\r\n*-1\r\n+PONG\r\n");

  const int resp3 = connect_socket(socket_path);
  assert(resp3 >= 0);
  std::string resp3_pending;
  send_command(resp3, {"HELLO", "3"});
  assert(read_reply(resp3, resp3_pending).starts_with("%7\r\n"));
  expect(resp3, resp3_pending, {"BLPOP", "resp3-timeout", "0.01"},
         "_\r\n");
  (void)::close(resp3);

  // Disconnecting removes the waiter; it must not consume a later item.
  const int abandoned = connect_socket(socket_path);
  assert(abandoned >= 0);
  send_command(abandoned, {"BLPOP", "abandoned", "0"});
  expect_no_reply(abandoned, std::chrono::milliseconds(20));
  (void)::close(abandoned);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  expect(producer, producer_pending, {"PING"}, "+PONG\r\n");
  expect(producer, producer_pending, {"RPUSH", "abandoned", "kept"},
         ":1\r\n");
  expect(producer, producer_pending, {"LPOP", "abandoned"},
         "$4\r\nkept\r\n");

  (void)::close(producer);
  (void)::close(second);
  (void)::close(first);
  (void)::kill(server, SIGTERM);
  (void)::waitpid(server, nullptr, 0);
  (void)::unlink(socket_path.c_str());
  std::puts("blocking list socket compatibility OK");
  return 0;
}
