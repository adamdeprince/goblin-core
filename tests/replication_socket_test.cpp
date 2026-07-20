#include "goblin/core/auth.hpp"
#include "goblin/core/replication.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
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

using namespace std::chrono_literals;
using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

struct Child {
  pid_t pid{-1};

  explicit Child(pid_t value) : pid(value) {}
  ~Child() { stop(); }
  void stop() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      for (int attempt = 0; attempt < 100; ++attempt) {
        if (::waitpid(pid, nullptr, WNOHANG) == pid) {
          pid = -1;
          return;
        }
        std::this_thread::sleep_for(10ms);
      }
      (void)::kill(pid, SIGKILL);
      (void)::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
};

[[nodiscard]] Child spawn_server(const char* binary,
                                 const std::vector<std::string>& args) {
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
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(binary));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(binary, argv.data());
    _exit(127);
  }
  return Child(pid);
}

[[nodiscard]] int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    (void)::close(fd);
    return -1;
  }
  return fd;
}

[[nodiscard]] int wait_for_uds(const std::string& path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = connect_uds(path); fd >= 0) return fd;
    std::this_thread::sleep_for(10ms);
  }
  return -1;
}

[[nodiscard]] std::uint16_t reserve_tcp_port() {
  for (int attempt = 0; attempt < 32; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) continue;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) == 0) {
      socklen_t size = sizeof(address);
      assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) ==
             0);
      const auto port = ntohs(address.sin_port);
      (void)::close(fd);
      return port;
    }
    (void)::close(fd);
    std::this_thread::sleep_for(1ms);
  }
  assert(false && "could not reserve a loopback TCP port");
  return 0;
}

void send_command(int fd, std::initializer_list<std::string_view> fields) {
  const auto wire = encode_command(
      std::span<const std::string_view>(fields.begin(), fields.size()));
  std::size_t offset = 0;
  while (offset < wire.size()) {
    const auto sent = ::send(fd, wire.data() + offset, wire.size() - offset, 0);
    assert(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

[[nodiscard]] std::string read_reply(int fd, std::string& pending) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    if (const auto end = reply_end(pending)) {
      std::string result = pending.substr(0, *end);
      pending.erase(0, *end);
      return result;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char bytes[16 * 1024];
    const auto received = ::recv(fd, bytes, sizeof(bytes), 0);
    assert(received > 0);
    pending.append(bytes, static_cast<std::size_t>(received));
  }
}

[[nodiscard]] std::string request(
    int fd, std::string& pending,
    std::initializer_list<std::string_view> fields) {
  send_command(fd, fields);
  return read_reply(fd, pending);
}

void wait_for_get(int fd, std::string& pending, std::string_view key,
                  std::string_view expected) {
  const std::string wanted = "$" + std::to_string(expected.size()) + "\r\n" +
                             std::string(expected) + "\r\n";
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (request(fd, pending, {"GET", key}) == wanted) return;
    std::this_thread::sleep_for(10ms);
  }
  assert(false && "replicated value did not arrive");
}

void wait_for_info(int fd, std::string& pending, std::string_view expected) {
  for (int attempt = 0; attempt < 1000; ++attempt) {
    if (request(fd, pending, {"INFO"}).find(expected) != std::string::npos) {
      return;
    }
    std::this_thread::sleep_for(10ms);
  }
  assert(false && "replica INFO state did not arrive");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace goblin::core;
  if (argc != 2) {
    std::cerr << "usage: replication_socket_test <goblin-core>\n";
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  const std::string primary_path = "/tmp/goblin-repl-primary-" + suffix + ".sock";
  const std::string replica_path = "/tmp/goblin-repl-replica-" + suffix + ".sock";
  const std::string chained_path = "/tmp/goblin-repl-chain-" + suffix + ".sock";
  const std::string auth_path = "/tmp/goblin-repl-auth-" + suffix + ".conf";
  const std::string password_path =
      "/tmp/goblin-repl-password-" + suffix + ".txt";
  const std::string snapshot_path =
      "/tmp/goblin-repl-primary-" + suffix + ".gcsn";
  const std::string late_primary_path =
      "/tmp/goblin-repl-late-primary-" + suffix + ".sock";
  const std::string early_replica_path =
      "/tmp/goblin-repl-early-replica-" + suffix + ".sock";
  const std::string ring_primary_path =
      "/tmp/goblin-repl-ring-primary-" + suffix + ".sock";
  const std::string ring_replica_path =
      "/tmp/goblin-repl-ring-replica-" + suffix + ".sock";
  const std::string ring_chained_path =
      "/tmp/goblin-repl-ring-chain-" + suffix + ".sock";
  const std::string replica_ring_path =
      "/tmp/goblin-repl-upstream-" + suffix + ".ring";
  const std::string chained_ring_path =
      "/tmp/goblin-repl-downstream-" + suffix + ".ring";
  const std::string ring_snapshot_path =
      "/tmp/goblin-repl-ring-primary-" + suffix + ".gcsn";
  for (const auto* path : {&primary_path, &replica_path, &chained_path,
                           &late_primary_path, &early_replica_path,
                           &ring_primary_path, &ring_replica_path,
                           &ring_chained_path, &replica_ring_path,
                           &chained_ring_path}) {
    (void)::unlink(path->c_str());
  }
  (void)::unlink(auth_path.c_str());
  (void)::unlink((auth_path + ".lock").c_str());
  (void)::unlink(password_path.c_str());
  (void)::unlink(snapshot_path.c_str());
  (void)::unlink(ring_snapshot_path.c_str());
  assert(upsert_auth_user(auth_path, "replicator", "secret") ==
         AuthUserUpdate::added);
  {
    std::ofstream password(password_path, std::ios::binary);
    password << "secret\n";
    assert(password.good());
  }

  {
    const auto primary_port = std::to_string(reserve_tcp_port());
    const auto replica_port = std::to_string(reserve_tcp_port());
    auto early_replica = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", early_replica_path,
                  "--port", replica_port, "--replica-uds",
                  late_primary_path});
    const int replica_client = wait_for_uds(early_replica_path);
    assert(replica_client >= 0);
    std::string replica_pending;
    wait_for_info(replica_client, replica_pending, "goblin_ready:0\r\n");

    auto late_primary = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", late_primary_path,
                  "--port", primary_port});
    const int primary_client = wait_for_uds(late_primary_path);
    assert(primary_client >= 0);
    std::string primary_pending;
    wait_for_info(replica_client, replica_pending, "replica_state:live\r\n");
    assert(request(primary_client, primary_pending,
                   {"SET", "late-primary", "connected"}) == "+OK\r\n");
    wait_for_get(replica_client, replica_pending, "late-primary", "connected");
    (void)::close(primary_client);
    (void)::close(replica_client);
  }

  {
    const auto primary_port = std::to_string(reserve_tcp_port());
    const auto replica_port = std::to_string(reserve_tcp_port());
    const auto chained_port = std::to_string(reserve_tcp_port());
    auto ring_replica = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", ring_replica_path,
                  "--port", replica_port, "--replica-ring",
                  replica_ring_path, "--ring", chained_ring_path, "64kb"});
    const int replica_client = wait_for_uds(ring_replica_path);
    assert(replica_client >= 0);
    std::string replica_pending;
    wait_for_info(replica_client, replica_pending, "goblin_ready:0\r\n");

    auto ring_primary = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", ring_primary_path, "--port",
                  primary_port, "--ring", replica_ring_path, "64kb"});
    int primary_client = wait_for_uds(ring_primary_path);
    assert(primary_client >= 0);
    std::string primary_pending;
    wait_for_info(replica_client, replica_pending, "replica_state:live\r\n");

    auto ring_chained = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", ring_chained_path, "--port",
                  chained_port, "--replica-ring", chained_ring_path});
    const int chained_client = wait_for_uds(ring_chained_path);
    assert(chained_client >= 0);
    std::string chained_pending;
    wait_for_info(chained_client, chained_pending, "replica_state:live\r\n");

    assert(request(primary_client, primary_pending,
                   {"SET", "ring-primary", "connected"}) == "+OK\r\n");
    wait_for_get(replica_client, replica_pending, "ring-primary", "connected");
    wait_for_get(chained_client, chained_pending, "ring-primary", "connected");

    assert(request(primary_client, primary_pending,
                   {"GOBLIN.SAVE", ring_snapshot_path, "NOACCEL"}) ==
           "+Background saving started\r\n");
    for (int attempt = 0; attempt < 1000 &&
                          !std::filesystem::exists(ring_snapshot_path);
         ++attempt) {
      std::this_thread::sleep_for(10ms);
    }
    assert(std::filesystem::exists(ring_snapshot_path));
    ring_primary.stop();
    (void)::close(primary_client);
    wait_for_info(replica_client, replica_pending, "goblin_ready:0\r\n");
    wait_for_info(chained_client, chained_pending, "goblin_ready:0\r\n");

    auto restarted_ring_primary = spawn_server(
        argv[1], {"--enable-sbe", "--unixsocket", ring_primary_path, "--port",
                  primary_port, "--ring", replica_ring_path, "64kb", "--load",
                  ring_snapshot_path});
    primary_client = wait_for_uds(ring_primary_path);
    assert(primary_client >= 0);
    wait_for_info(replica_client, replica_pending, "replica_state:live\r\n");
    wait_for_info(chained_client, chained_pending, "replica_state:live\r\n");
    assert(request(primary_client, primary_pending,
                   {"SET", "ring-restart", "safe"}) == "+OK\r\n");
    wait_for_get(replica_client, replica_pending, "ring-restart", "safe");
    wait_for_get(chained_client, chained_pending, "ring-restart", "safe");

    (void)::close(primary_client);
    (void)::close(chained_client);
    (void)::close(replica_client);
  }

  {
    const auto primary_port = std::to_string(reserve_tcp_port());
    const auto replica_port = std::to_string(reserve_tcp_port());
    const auto chained_port = std::to_string(reserve_tcp_port());
    auto primary = spawn_server(
        argv[1], {"--enable-sbe", "--auth-file", auth_path, "--unixsocket",
                  primary_path, "--port", primary_port});
    const int primary_client = wait_for_uds(primary_path);
    assert(primary_client >= 0);

    auto replica = spawn_server(
        argv[1], {"--enable-sbe", "--auth-file", auth_path, "--unixsocket",
                  replica_path, "--port", replica_port, "--replica-uds",
                  primary_path, "--replica-auth-user", "replicator",
                  "--replica-auth-password-file", password_path});
    const int replica_client = wait_for_uds(replica_path);
    assert(replica_client >= 0);

    // Start a replica-of-replica before writes begin. Both downstream servers
    // adopt the primary lineage at offset zero, then relay the same batches.
    auto chained = spawn_server(
        argv[1], {"--enable-sbe", "--auth-file", auth_path, "--unixsocket",
                  chained_path, "--port", chained_port, "--replica-uds",
                  replica_path, "--replica-auth-user", "replicator",
                  "--replica-auth-password-file", password_path});
    const int chained_client = wait_for_uds(chained_path);
    assert(chained_client >= 0);

    std::string primary_pending;
    std::string replica_pending;
    std::string chained_pending;
    assert(request(primary_client, primary_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    assert(request(replica_client, replica_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    assert(request(chained_client, chained_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");

    const int observer = connect_uds(primary_path);
    assert(observer >= 0);
    std::string observer_pending;
    assert(request(observer, observer_pending, {"GOBLIN.FIREHOSE"}) ==
           "-NOAUTH Authentication required.\r\n");
    assert(request(observer, observer_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    send_command(observer, {"GOBLIN.FIREHOSE"});
    std::string error;
    const auto hello = decode_firehose_hello(
        read_reply(observer, observer_pending), error);
    assert(hello && hello->offset == 0);

    assert(request(primary_client, primary_pending, {"MULTI"}) == "+OK\r\n");
    assert(request(primary_client, primary_pending,
                   {"SET", "alpha", "one"}) == "+QUEUED\r\n");
    assert(request(primary_client, primary_pending,
                   {"HSET", "hash", "field", "value"}) == "+QUEUED\r\n");
    assert(request(primary_client, primary_pending,
                   {"LPUSH", "list", "a", "b", "c"}) == "+QUEUED\r\n");
    const auto exec_reply = request(primary_client, primary_pending, {"EXEC"});
    assert(!exec_reply.empty() && exec_reply.front() == '*');

    // EXEC is one atomic firehose frame, even though each independently
    // compactable mutation advances the logical offset.
    const auto transaction_batch = decode_firehose_batch(
        read_reply(observer, observer_pending), error);
    assert(transaction_batch && transaction_batch->offset == 1);
    assert(transaction_batch->mutations.size() == 3);

    wait_for_get(replica_client, replica_pending, "alpha", "one");
    wait_for_get(chained_client, chained_pending, "alpha", "one");
    assert(request(replica_client, replica_pending,
                   {"HGET", "hash", "field"}) == "$5\r\nvalue\r\n");
    assert(request(chained_client, chained_pending, {"LLEN", "list"}) ==
           ":3\r\n");

    // Direct SBE writes enter the same transport-neutral firehose.
    auto sbe = SbeSocketClient::open(SbeSocketEndpoint::unix_domain(primary_path),
                                     5s);
    assert(sbe);
    assert(sbe->set("sbe-key", "sbe-value").ok);
    wait_for_get(replica_client, replica_pending, "sbe-key", "sbe-value");
    wait_for_get(chained_client, chained_pending, "sbe-key", "sbe-value");

    // Random source choices are canonicalized to exact member removals.
    assert(request(primary_client, primary_pending,
                   {"SADD", "choices", "a", "b", "c", "d"}) == ":4\r\n");
    const auto pop = request(primary_client, primary_pending,
                             {"SPOP", "choices", "2"});
    assert(!pop.empty() && pop.front() == '*');
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (request(chained_client, chained_pending, {"SCARD", "choices"}) ==
          ":2\r\n") {
        break;
      }
      assert(attempt != 499);
      std::this_thread::sleep_for(10ms);
    }
    for (const std::string_view member : {"a", "b", "c", "d"}) {
      const auto source = request(primary_client, primary_pending,
                                  {"SISMEMBER", "choices", member});
      assert(request(replica_client, replica_pending,
                     {"SISMEMBER", "choices", member}) == source);
      assert(request(chained_client, chained_pending,
                     {"SISMEMBER", "choices", member}) == source);
    }

    assert(request(replica_client, replica_pending,
                   {"SET", "forbidden", "write"})
               .starts_with("-READONLY "));
    assert(request(replica_client, replica_pending, {"ROLE"})
               .find("$5\r\nslave\r\n") != std::string::npos);
    assert(request(chained_client, chained_pending, {"ROLE"})
               .find("$5\r\nslave\r\n") != std::string::npos);

    const auto live_info = request(replica_client, replica_pending, {"INFO"});
    assert(live_info.find("goblin_ready:1\r\n") != std::string::npos);
    assert(live_info.find("replica_state:live\r\n") != std::string::npos);

    // Restarting an upstream at the exact durable offset is gap-free without
    // Kafka. The direct replica remains readable but advertises not-ready while
    // reconnecting. Its chained subscriber is disconnected and independently
    // reconnects, so it cannot silently miss a recovery interval.
    assert(request(primary_client, primary_pending,
                   {"GOBLIN.SAVE", snapshot_path, "NOACCEL"}) ==
           "+Background saving started\r\n");
    for (int attempt = 0; attempt < 1000 &&
                          !std::filesystem::exists(snapshot_path);
         ++attempt) {
      std::this_thread::sleep_for(10ms);
    }
    assert(std::filesystem::exists(snapshot_path));
    primary.stop();
    (void)::close(primary_client);
    wait_for_info(replica_client, replica_pending, "goblin_ready:0\r\n");
    assert(request(replica_client, replica_pending, {"GET", "alpha"}) ==
           "$3\r\none\r\n");

    auto restarted_primary = spawn_server(
        argv[1], {"--enable-sbe", "--auth-file", auth_path, "--unixsocket",
                  primary_path, "--port", primary_port, "--load",
                  snapshot_path});
    const int restarted_primary_client = wait_for_uds(primary_path);
    assert(restarted_primary_client >= 0);
    std::string restarted_primary_pending;
    assert(request(restarted_primary_client, restarted_primary_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    wait_for_info(replica_client, replica_pending, "replica_state:live\r\n");
    wait_for_info(chained_client, chained_pending, "replica_state:live\r\n");
    const auto recovered_info =
        request(replica_client, replica_pending, {"INFO"});
    assert(recovered_info.find("replica_successful_reconnects:1\r\n") !=
           std::string::npos);

    assert(request(restarted_primary_client, restarted_primary_pending,
                   {"SET", "after-restart", "safe"}) == "+OK\r\n");
    wait_for_get(replica_client, replica_pending, "after-restart", "safe");
    wait_for_get(chained_client, chained_pending, "after-restart", "safe");

    (void)::close(observer);
    (void)::close(restarted_primary_client);
    (void)::close(chained_client);
    (void)::close(replica_client);
  }

  for (const auto* path : {&primary_path, &replica_path, &chained_path,
                           &late_primary_path, &early_replica_path,
                           &ring_primary_path, &ring_replica_path,
                           &ring_chained_path, &replica_ring_path,
                           &chained_ring_path}) {
    (void)::unlink(path->c_str());
  }
  (void)::unlink(auth_path.c_str());
  (void)::unlink((auth_path + ".lock").c_str());
  (void)::unlink(password_path.c_str());
  (void)::unlink(snapshot_path.c_str());
  (void)::unlink(ring_snapshot_path.c_str());
  std::cout << "Primary, chained replica, transaction, and SBE replication OK\n";
  return 0;
}
