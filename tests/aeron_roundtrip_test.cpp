#include "goblin/core/aeron_client.hpp"
#include "goblin/core/aeron_transport.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using goblin::core::SbeAeronClient;
using goblin::core::aeron::AeronClient;
using goblin::core::aeron::ChannelConfig;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "goblin-aeron-XXXXXX")
            .string();
    if (::mkdtemp(pattern.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = std::move(pattern);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

class ChildProcess {
 public:
  ChildProcess() = default;
  explicit ChildProcess(pid_t pid) noexcept : pid_(pid) {}
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ChildProcess(ChildProcess&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)) {}

  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      stop();
      pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
  }

  ~ChildProcess() { stop(); }

  [[nodiscard]] bool running() noexcept {
    if (pid_ < 0) return false;
    int status = 0;
    const pid_t result = ::waitpid(pid_, &status, WNOHANG);
    if (result == 0) return true;
    if (result == pid_) pid_ = -1;
    return false;
  }

  void stop() noexcept {
    if (pid_ < 0) return;
    (void)::kill(pid_, SIGTERM);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      int status = 0;
      const pid_t result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_ || result < 0) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(10ms);
    }
    (void)::kill(pid_, SIGKILL);
    int status = 0;
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

 private:
  pid_t pid_{-1};
};

[[nodiscard]] std::uint16_t reserve_loopback_port(int socket_type) {
  const int fd = ::socket(AF_INET, socket_type, 0);
  if (fd < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
    (void)::close(fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    (void)::close(fd);
    return 0;
  }
  const auto port = ntohs(address.sin_port);
  (void)::close(fd);
  return port;
}

[[nodiscard]] ChildProcess start_driver(const char* executable,
                                        const std::string& directory) {
  const pid_t pid = ::fork();
  if (pid < 0) throw std::runtime_error("fork Media Driver failed");
  if (pid == 0) {
    const std::string directory_option = "-Daeron.dir=" + directory;
    ::execl(executable, executable, directory_option.c_str(),
            "-Daeron.dir.delete.on.start=true",
            "-Daeron.dir.delete.on.shutdown=true",
            static_cast<char*>(nullptr));
    _exit(127);
  }
  return ChildProcess(pid);
}

[[nodiscard]] bool await_driver(ChildProcess& driver,
                                const std::string& directory,
                                std::string_view role) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!std::filesystem::exists(
      std::filesystem::path(directory) / "cnc.dat")) {
    if (!driver.running() || std::chrono::steady_clock::now() >= deadline) {
      std::fprintf(stderr,
                   "aeron_roundtrip_test: %.*s Media Driver did not become ready\n",
                   static_cast<int>(role.size()), role.data());
      return false;
    }
    std::this_thread::sleep_for(10ms);
  }
  return true;
}

[[nodiscard]] bool create_auth_file(const char* executable,
                                    const std::string& path) {
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    ::execl(executable, executable, "--file", path.c_str(), "add", "default",
            "aeron-secret", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  return ::waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

[[nodiscard]] ChildProcess start_server(
    const char* executable, const std::string& directory,
    std::uint16_t request_port, std::uint16_t response_port,
    std::uint16_t ordinary_port, const std::string& auth_file) {
  const pid_t pid = ::fork();
  if (pid < 0) throw std::runtime_error("fork goblin-core failed");
  if (pid == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) (void)::dup2(null_fd, STDOUT_FILENO);
    const std::string request =
        "127.0.0.1:" + std::to_string(request_port);
    const std::string response =
        "127.0.0.1:" + std::to_string(response_port);
    const std::string ordinary = std::to_string(ordinary_port);
    ::execl(executable, executable, "--enable-sbe", "--aeron-dir",
            directory.c_str(), "--aeron-ipc", "1001", "1002",
            "--aeron-udp", request.c_str(), "2001", response.c_str(),
            "2002", "--port", ordinary.c_str(), "--auth-file",
            auth_file.c_str(),
            static_cast<char*>(nullptr));
    _exit(127);
  }
  return ChildProcess(pid);
}

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::fprintf(stderr, "aeron_roundtrip_test: %.*s\n",
                 static_cast<int>(message.size()), message.data());
  }
  return condition;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr,
                 "usage: %s <goblin-core> <aeronmd_s-or-aeronmd> "
                 "<goblin-core-auth>\n",
                 argv[0]);
    return 2;
  }

  try {
    TemporaryDirectory server_directory;
    TemporaryDirectory client_directory;
    TemporaryDirectory auth_directory;
    const std::string auth_file =
        (std::filesystem::path(auth_directory.path()) / "goblin.auth")
            .string();
    if (!create_auth_file(argv[3], auth_file)) {
      std::fprintf(stderr,
                   "aeron_roundtrip_test: could not create auth file\n");
      return 1;
    }
    ChildProcess server_driver = start_driver(argv[2], server_directory.path());
    ChildProcess client_driver = start_driver(argv[2], client_directory.path());
    if (!await_driver(server_driver, server_directory.path(), "server") ||
        !await_driver(client_driver, client_directory.path(), "client")) {
      return 1;
    }

    const auto request_port = reserve_loopback_port(SOCK_DGRAM);
    const auto response_port = reserve_loopback_port(SOCK_DGRAM);
    const auto ordinary_port = reserve_loopback_port(SOCK_STREAM);
    if (request_port == 0 || response_port == 0 || ordinary_port == 0 ||
        request_port == response_port) {
      std::fprintf(stderr,
                   "aeron_roundtrip_test: could not reserve loopback ports\n");
      return 1;
    }
    ChildProcess server = start_server(argv[1], server_directory.path(),
                                       request_port, response_port,
                                       ordinary_port, auth_file);

    const ChannelConfig ipc = ChannelConfig::ipc(1001, 1002);
    std::string error;
    auto ipc_client = SbeAeronClient::open(
        ipc, 10s, 128U * 1024U, server_directory.path(), &error);
    if (!ipc_client) {
      std::fprintf(stderr, "aeron_roundtrip_test: IPC SBE connect: %s\n",
                   error.c_str());
      return 1;
    }
    const std::string large_value(32U * 1024U, 'i');
    if (!expect(ipc_client->ping(), "IPC SBE PING failed") ||
        !expect(ipc_client->set("aeron:ipc:large", large_value).ok,
                "IPC fragmented SBE SET failed") ||
        !expect(ipc_client->get("aeron:ipc:large") == large_value,
                "IPC fragmented SBE GET failed")) {
      return 1;
    }

    error.clear();
    auto ipc_peer = SbeAeronClient::open(
        ipc, 10s, 128U * 1024U, server_directory.path(), &error);
    if (!ipc_peer ||
        !expect(ipc_peer->get("aeron:ipc:large") == large_value,
                "second correlated IPC client failed")) {
      std::fprintf(stderr, "aeron_roundtrip_test: IPC peer: %s\n",
                   error.c_str());
      return 1;
    }

    constexpr std::string_view kChannel = "aeron:qualification";
    constexpr std::string_view kPayload = "correlated-push";
    const std::array<std::string_view, 1> channels{kChannel};
    const auto acknowledgements = ipc_peer->subscribe(channels);
    if (!expect(acknowledgements.size() == 1,
                "IPC SBE SUBSCRIBE acknowledgement failed") ||
        !expect(ipc_client->publish(kChannel, kPayload) == 1,
                "IPC SBE PUBLISH subscriber count was not one")) {
      return 1;
    }
    const auto message = ipc_peer->read_pubsub();
    if (!expect(message.kind == goblin::core::PubSubKind::message,
                "unexpected Aeron Pub/Sub push kind") ||
        !expect(message.channel == kChannel,
                "Aeron Pub/Sub channel mismatch") ||
        !expect(message.payload == kPayload,
                "Aeron Pub/Sub payload mismatch")) {
      return 1;
    }

    const ChannelConfig udp = ChannelConfig::udp(
        "127.0.0.1:" + std::to_string(request_port), 2001,
        "127.0.0.1:" + std::to_string(response_port), 2002);
    error.clear();
    auto resp = AeronClient::open(udp, 10s, client_directory.path(), &error);
    if (!resp) {
      std::fprintf(stderr, "aeron_roundtrip_test: UDP RESP connect: %s\n",
                   error.c_str());
      return 1;
    }
    if (!expect(resp->command({"PING"}) ==
                    "-NOAUTH Authentication required.\r\n",
                "UDP RESP authentication was not required") ||
        !expect(resp->command({"AUTH", "default", "aeron-secret"}) ==
                    "+OK\r\n",
                "UDP RESP AUTH failed") ||
        !expect(resp->command({"PING"}) == "+PONG\r\n",
                "UDP RESP PING failed") ||
        !expect(resp->command({"SET", "aeron:udp:key", "value"}) ==
                    "+OK\r\n",
                "UDP RESP SET failed") ||
        !expect(resp->command({"GET", "aeron:udp:key"}) ==
                    "$5\r\nvalue\r\n",
                "UDP RESP GET failed")) {
      return 1;
    }
    const auto hello = resp->command({"HELLO", "3"});
    if (!expect(hello && !hello->empty() && (*hello)[0] == '%',
                "UDP RESP3 HELLO failed")) {
      return 1;
    }
    const auto info = resp->command({"INFO"});
    if (!expect(info && info->find("aeron_support:1\r\n") !=
                            std::string::npos,
                "INFO lacks aeron_support:1")) {
      return 1;
    }

    constexpr unsigned kPipelineRequests = 128;
    const std::string_view ping[] = {"PING"};
    for (unsigned i = 0; i < kPipelineRequests; ++i) {
      resp->send_pipelined(ping);
    }
    for (unsigned i = 0; i < kPipelineRequests; ++i) {
      if (!expect(resp->read_reply() == "+PONG\r\n",
                  "UDP pipelined RESP PING failed")) {
        return 1;
      }
    }

    error.clear();
    auto udp_sbe = SbeAeronClient::open(
        udp, 10s, 128U * 1024U, client_directory.path(), &error);
    if (!udp_sbe || !expect(udp_sbe->ping(), "UDP SBE PING failed") ||
        !expect(udp_sbe->get("aeron:udp:key") == "value",
                "UDP SBE cross-client GET failed")) {
      std::fprintf(stderr, "aeron_roundtrip_test: UDP SBE: %s\n",
                   error.c_str());
      return 1;
    }

    // Repeated image creation/removal exercises the server's per-client
    // response-publication cleanup and correlation-id bookkeeping.
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      error.clear();
      auto churn = SbeAeronClient::open(
          ipc, 10s, 128U * 1024U, server_directory.path(), &error);
      if (!churn || !expect(churn->ping(), "IPC client churn PING failed")) {
        std::fprintf(stderr, "aeron_roundtrip_test: churn client: %s\n",
                     error.c_str());
        return 1;
      }
    }
    if (!expect(ipc_client->ping(),
                "original IPC client failed after connection churn")) {
      return 1;
    }

    std::puts("Aeron IPC/UDP RESP + SBE + Pub/Sub round trip OK");
    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "aeron_roundtrip_test: %s\n", exception.what());
    return 1;
  }
}
