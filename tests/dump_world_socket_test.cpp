#include "goblin/core/command.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/store.hpp"
#include "socket_test_utils.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <span>
#include <sstream>
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

using namespace std::chrono_literals;
using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

struct Child {
  pid_t pid{-1};

  explicit Child(pid_t child) : pid(child) {}
  ~Child() {
    if (pid <= 0) return;
    (void)::kill(pid, SIGTERM);
    (void)::waitpid(pid, nullptr, 0);
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
};

[[nodiscard]] int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0) {
    return fd;
  }
  (void)::close(fd);
  return -1;
}

[[nodiscard]] int wait_for_uds(const std::string& path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = connect_uds(path); fd >= 0) return fd;
    std::this_thread::sleep_for(10ms);
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

void send_command(int fd, std::initializer_list<std::string_view> fields) {
  send_all(fd, encode_command(
                   std::span<const std::string_view>(fields.begin(),
                                                     fields.size())));
}

void fill(int fd, std::string& pending, std::size_t bytes) {
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (pending.size() < bytes) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char buffer[64 * 1024];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

[[nodiscard]] std::string take(int fd, std::string& pending,
                               std::size_t bytes) {
  fill(fd, pending, bytes);
  std::string result = pending.substr(0, bytes);
  pending.erase(0, bytes);
  return result;
}

[[nodiscard]] std::string take_line(int fd, std::string& pending) {
  for (;;) {
    if (const auto end = pending.find("\r\n"); end != std::string::npos) {
      std::string result = pending.substr(0, end);
      pending.erase(0, end + 2);
      return result;
    }
    fill(fd, pending, pending.size() + 1);
  }
}

[[nodiscard]] std::string read_reply(int fd, std::string& pending) {
  for (;;) {
    if (const auto end = reply_end(pending)) {
      std::string result = pending.substr(0, *end);
      pending.erase(0, *end);
      return result;
    }
    fill(fd, pending, pending.size() + 1);
  }
}

[[nodiscard]] std::string request(
    int fd, std::string& pending,
    std::initializer_list<std::string_view> fields) {
  send_command(fd, fields);
  return read_reply(fd, pending);
}

[[nodiscard]] std::string execute(
    goblin::core::Store& store,
    std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  return goblin::core::execute_command(store, *parsed.command);
}

[[nodiscard]] std::string read_dump(int fd, std::string& pending,
                                    std::size_t& chunks) {
  assert(take(fd, pending, 4) == "$?\r\n");
  std::string snapshot;
  chunks = 0;
  for (;;) {
    const std::string line = take_line(fd, pending);
    assert(!line.empty() && line.front() == ';');
    std::size_t size = 0;
    const auto [end, error] =
        std::from_chars(line.data() + 1, line.data() + line.size(), size);
    assert(error == std::errc{} && end == line.data() + line.size());
    if (size == 0) break;
    std::string chunk = take(fd, pending, size + 2);
    assert(chunk.ends_with("\r\n"));
    chunk.resize(size);
    snapshot.append(chunk);
    ++chunks;
  }
  return snapshot;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: dump_world_socket_test <goblin-core>\n");
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  const std::string socket_path = "/tmp/goblin-dump-world-" + suffix + ".sock";
  const std::string save_path = "/tmp/goblin-dump-world-" + suffix + ".gcsn";
  (void)::unlink(socket_path.c_str());
  (void)::unlink(save_path.c_str());
  const auto port = goblin::test::reserve_loopback_tcp_port();
  assert(port != 0);
  const std::string port_text = std::to_string(port);

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    if (::getenv("GOBLIN_TEST_LOG") == nullptr) {
      const int null_fd = ::open("/dev/null", O_WRONLY);
      if (null_fd >= 0) {
        (void)::dup2(null_fd, STDOUT_FILENO);
        (void)::dup2(null_fd, STDERR_FILENO);
      }
    }
    ::execl(argv[1], argv[1], "--unixsocket", socket_path.c_str(), "--port",
            port_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  Child server(pid);

  const int control = wait_for_uds(socket_path);
  assert(control >= 0);
  std::string control_pending;
  assert(request(control, control_pending, {"SET", "frozen", "before"}) ==
         "+OK\r\n");
  assert(request(control, control_pending,
                 {"HSET", "hash", "field", "value"}) == ":1\r\n");
  assert(request(control, control_pending,
                 {"RPUSH", "list", "alpha", "beta"}) == ":2\r\n");
  assert(request(control, control_pending,
                 {"SADD", "set", "one", "two"}) == ":2\r\n");
  assert(request(control, control_pending,
                 {"ZADD", "leaders", "42", "alice"}) == ":1\r\n");
  assert(request(control, control_pending,
                 {"ARSET", "array", "9000", "sparse"}) == ":1\r\n");

  // Keep the child alive behind pipe/socket backpressure long enough to prove
  // the parent continues serving and enforces the single snapshot slot.
  const std::string value(4096, 'v');
  std::string pipeline;
  constexpr int kPayloadKeys = 2048;
  for (int index = 0; index < kPayloadKeys; ++index) {
    const std::string key = "payload:" + std::to_string(index);
    const std::string_view fields[] = {"SET", key, value};
    pipeline.append(encode_command(fields));
  }
  send_all(control, pipeline);
  for (int index = 0; index < kPayloadKeys; ++index) {
    assert(read_reply(control, control_pending) == "+OK\r\n");
  }

  const int dump = connect_uds(socket_path);
  assert(dump >= 0);
  std::string dump_pending;
  assert(request(dump, dump_pending, {"GOBLIN.DUMPWORLD"}) ==
         "-ERR GOBLIN.DUMPWORLD requires HELLO 3\r\n");
  const auto hello = request(dump, dump_pending, {"HELLO", "3"});
  assert(hello.starts_with("%7\r\n"));
  assert(request(dump, dump_pending, {"GOBLIN.DUMPWORLD", "BOGUS"}) ==
         "-ERR syntax error, expected ACCEL or NOACCEL\r\n");

  send_command(dump, {"GOBLIN.DUMPWORLD"});
  assert(take(dump, dump_pending, 4) == "$?\r\n");

  const auto save_reply =
      request(control, control_pending, {"GOBLIN.SAVE", save_path});
  assert(save_reply.find("background save already in progress") !=
         std::string::npos);
  assert(request(control, control_pending, {"DEL", "frozen"}) == ":1\r\n");
  assert(request(control, control_pending, {"SET", "post-fork", "new"}) ==
         "+OK\r\n");
  assert(request(control, control_pending, {"PING"}) == "+PONG\r\n");

  // Put the already-consumed streamed-string marker back for the common reader.
  dump_pending.insert(0, "$?\r\n");
  std::size_t chunks = 0;
  const std::string snapshot = read_dump(dump, dump_pending, chunks);
  assert(chunks > 1);
  assert(snapshot.starts_with("GCSN"));

  pollfd closed{.fd = dump, .events = POLLIN, .revents = 0};
  assert(::poll(&closed, 1, 5000) > 0);
  char trailing = 0;
  assert(::recv(dump, &trailing, 1, 0) == 0);

  goblin::core::Store restored;
  std::istringstream input(snapshot, std::ios::binary);
  const auto loaded = restored.load(input);
  assert(loaded.keys >= static_cast<std::size_t>(kPayloadKeys + 6));
  assert(execute(restored, {"GET", "frozen"}) == "$6\r\nbefore\r\n");
  assert(execute(restored, {"GET", "post-fork"}) == "$-1\r\n");
  assert(execute(restored, {"HGET", "hash", "field"}) ==
         "$5\r\nvalue\r\n");
  assert(execute(restored, {"LRANGE", "list", "0", "-1"}) ==
         "*2\r\n$5\r\nalpha\r\n$4\r\nbeta\r\n");
  assert(execute(restored, {"SISMEMBER", "set", "two"}) == ":1\r\n");
  assert(execute(restored, {"ZSCORE", "leaders", "alice"}) ==
         "$2\r\n42\r\n");
  assert(execute(restored, {"ARGET", "array", "9000"}) ==
         "$6\r\nsparse\r\n");
  assert(execute(restored, {"GET", "payload:2047"}) ==
         "$4096\r\n" + value + "\r\n");

  // The dump connection is one-shot; other clients remain usable.
  assert(request(control, control_pending, {"PING"}) == "+PONG\r\n");

  (void)::close(dump);
  (void)::close(control);
  (void)::unlink(socket_path.c_str());
  (void)::unlink(save_path.c_str());
  std::puts("GOBLIN.DUMPWORLD streamed snapshot OK");
  return 0;
}
