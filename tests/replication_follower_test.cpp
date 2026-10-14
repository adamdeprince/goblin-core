#include "replication_follower.hpp"
#include "goblin/core/ring_client.hpp"

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

std::optional<std::string> read_request(int fd, std::string& pending) {
  for (;;) {
    if (const auto end = goblin::core::ring::reply_end(pending)) {
      auto request = pending.substr(0, *end);
      pending.erase(0, *end);
      return request;
    }
    char bytes[1024];
    const auto received = ::recv(fd, bytes, sizeof(bytes), 0);
    if (received <= 0) return std::nullopt;
    pending.append(bytes, static_cast<std::size_t>(received));
  }
}

bool send_reply(int fd, std::string_view reply) {
  while (!reply.empty()) {
    const auto sent = ::send(fd, reply.data(), reply.size(), 0);
    if (sent <= 0) return false;
    reply.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;
  using namespace goblin::core;
  std::signal(SIGPIPE, SIG_IGN);
  const std::string path =
      "/tmp/goblin-repl-deadline-" + std::to_string(::getpid()) + ".sock";
  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  assert(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == 0);
  assert(::listen(listener, 1) == 0);
  const ReplicationState state{
      .id = make_replication_id(), .offset = 42, .valid = true};
  std::optional<FirehoseHello> hello;
  std::string error;
  {
    std::jthread upstream([&] {
      const int client = ::accept(listener, nullptr, nullptr);
      assert(client >= 0);
      const timeval receive_timeout{.tv_sec = 5, .tv_usec = 0};
      assert(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                          sizeof(receive_timeout)) == 0);
      std::string pending;
      const auto auth = read_request(client, pending);
      assert(auth == "*3\r\n$4\r\nAUTH\r\n$10\r\nreplicator\r\n$6\r\nsecret\r\n");
      // Both replies exceed the transport deadline. The second models an
      // upstream handling another client's password hash before its firehose.
      std::this_thread::sleep_for(350ms);
      if (send_reply(client, "+OK\r\n")) {
        const auto firehose = read_request(client, pending);
        if (firehose) {
          assert(*firehose == "*1\r\n$15\r\nGOBLIN.FIREHOSE\r\n");
          std::this_thread::sleep_for(350ms);
          (void)send_reply(client, encode_firehose_hello(state));
        }
      }
      (void)::close(client);
    });
    try {
      const ReplicaAuthConfig auth{.username = "replicator", .password = "secret"};
      ReplicationFollowerRuntime follower(
          ReplicaUdsConfig{.path = path}, 1024 * 1024, &auth, 100ms);
      hello = follower.hello();
    } catch (const std::exception& exception) {
      error = exception.what();
    }
  }
  (void)::close(listener);
  (void)::unlink(path.c_str());
  if (!error.empty()) std::cerr << error << '\n';
  assert(hello && "authenticated replication replies need a longer deadline");
  assert(hello->id == state.id && hello->offset == state.offset);
}
