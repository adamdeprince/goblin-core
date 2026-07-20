#pragma once

// Non-blocking TCP/Unix-domain stream adapter for BasicSbeClient. The SBE
// framing layer above this transport handles arbitrary recv() boundaries.

#include "goblin/core/ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace goblin::core {

struct SbeSocketEndpoint {
  enum class Kind : std::uint8_t { tcp, unix_domain };

  Kind kind{Kind::tcp};
  std::string address;
  std::uint16_t port{0};

  [[nodiscard]] static SbeSocketEndpoint tcp(std::string host,
                                              std::uint16_t port) {
    return {.kind = Kind::tcp, .address = std::move(host), .port = port};
  }

  [[nodiscard]] static SbeSocketEndpoint unix_domain(std::string path) {
    return {.kind = Kind::unix_domain, .address = std::move(path), .port = 0};
  }
};

class SocketSbeTransport {
 public:
  using ms = std::chrono::milliseconds;

  SocketSbeTransport(const SocketSbeTransport&) = delete;
  SocketSbeTransport& operator=(const SocketSbeTransport&) = delete;

  SocketSbeTransport(SocketSbeTransport&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        buffer_size_(other.buffer_size_),
        inbound_(std::move(other.inbound_)),
        consume_offset_(other.consume_offset_) {}

  SocketSbeTransport& operator=(SocketSbeTransport&& other) noexcept {
    if (this != &other) {
      close_fd();
      fd_ = std::exchange(other.fd_, -1);
      buffer_size_ = other.buffer_size_;
      inbound_ = std::move(other.inbound_);
      consume_offset_ = other.consume_offset_;
    }
    return *this;
  }

  ~SocketSbeTransport() { close_fd(); }

  [[nodiscard]] static std::optional<SocketSbeTransport> open(
      const SbeSocketEndpoint& endpoint, ms timeout = ms(5000),
      std::size_t buffer_size = 64 * 1024, std::string* error = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int fd = -1;
    std::string failure;
    if (endpoint.kind == SbeSocketEndpoint::Kind::tcp) {
      fd = connect_tcp(endpoint.address, endpoint.port, deadline, failure);
    } else {
      fd = connect_unix(endpoint.address, deadline, failure);
    }
    if (fd < 0) {
      if (error != nullptr) {
        *error = std::move(failure);
      }
      return std::nullopt;
    }
    return SocketSbeTransport(fd, buffer_size);
  }

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) {
    while (!bytes.empty()) {
      const ssize_t sent =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (stop()) {
          return false;
        }
        pollfd event{.fd = fd_, .events = POLLOUT, .revents = 0};
        (void)::poll(&event, 1, 0);
        ring::cpu_relax();
        continue;
      }
      throw std::runtime_error(socket_error("send"));
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string_view> peek() {
    if (consume_offset_ < inbound_.size()) {
      return std::string_view(inbound_).substr(consume_offset_);
    }
    inbound_.clear();
    consume_offset_ = 0;

    char chunk[16 * 1024];
    for (;;) {
      const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (received > 0) {
        inbound_.append(chunk, static_cast<std::size_t>(received));
        return std::string_view(inbound_);
      }
      if (received == 0) {
        throw std::runtime_error("upstream SBE socket closed");
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::nullopt;
      }
      throw std::runtime_error(socket_error("recv"));
    }
  }

  void pop() noexcept {
    consume_offset_ = inbound_.size();
  }

  void wait_for_record() const noexcept { ring::cpu_relax(); }

  [[nodiscard]] std::size_t send_capacity() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] std::size_t receive_capacity() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] std::size_t max_message_bytes() const noexcept {
    return std::numeric_limits<std::uint32_t>::max();
  }
  [[nodiscard]] std::size_t buffer_size_hint() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] int native_handle() const noexcept { return fd_; }
  [[nodiscard]] int release_native_handle() noexcept {
    return std::exchange(fd_, -1);
  }

 private:
  SocketSbeTransport(int fd, std::size_t buffer_size)
      : fd_(fd), buffer_size_(buffer_size == 0 ? 64 * 1024 : buffer_size) {
    inbound_.reserve(buffer_size_);
  }

  [[nodiscard]] static std::string socket_error(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
  }

  [[nodiscard]] static bool set_nonblocking(int fd, std::string& error) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      error = socket_error("fcntl(O_NONBLOCK)");
      return false;
    }
    return true;
  }

  [[nodiscard]] static bool finish_connect(
      int fd, std::chrono::steady_clock::time_point deadline,
      std::string& error) {
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        error = "connect timed out";
        return false;
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int wait_ms = static_cast<int>(std::clamp<long long>(
          remaining.count(), 1, std::numeric_limits<int>::max()));
      pollfd event{.fd = fd, .events = POLLOUT, .revents = 0};
      const int ready = ::poll(&event, 1, wait_ms);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0) {
        error = socket_error("poll");
        return false;
      }
      if (ready == 0) {
        continue;
      }
      int connect_error = 0;
      socklen_t length = sizeof(connect_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &length) != 0) {
        error = socket_error("getsockopt(SO_ERROR)");
        return false;
      }
      if (connect_error != 0) {
        error = "connect: " + std::string(std::strerror(connect_error));
        return false;
      }
      return true;
    }
  }

  [[nodiscard]] static bool connect_address(
      int fd, const sockaddr* address, socklen_t length,
      std::chrono::steady_clock::time_point deadline, std::string& error) {
    if (::connect(fd, address, length) == 0) {
      return true;
    }
    if (errno != EINPROGRESS) {
      error = socket_error("connect");
      return false;
    }
    return finish_connect(fd, deadline, error);
  }

  [[nodiscard]] static int connect_tcp(
      std::string_view host, std::uint16_t port,
      std::chrono::steady_clock::time_point deadline, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string host_storage(host);
    const std::string port_storage = std::to_string(port);
    const int result = ::getaddrinfo(host_storage.c_str(), port_storage.c_str(),
                                     &hints, &addresses);
    if (result != 0) {
      error = std::string("getaddrinfo: ") + ::gai_strerror(result);
      return -1;
    }

    int connected = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
      const int fd =
          ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd < 0) {
        error = socket_error("socket");
        continue;
      }
      int one = 1;
      (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (!set_nonblocking(fd, error) ||
          !connect_address(fd, address->ai_addr,
                           static_cast<socklen_t>(address->ai_addrlen), deadline,
                           error)) {
        ::close(fd);
        continue;
      }
      connected = fd;
      break;
    }
    ::freeaddrinfo(addresses);
    return connected;
  }

  [[nodiscard]] static int connect_unix(
      std::string_view path, std::chrono::steady_clock::time_point deadline,
      std::string& error) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
      error = "Unix socket path is too long";
      return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      error = socket_error("socket(AF_UNIX)");
      return -1;
    }
    if (!set_nonblocking(fd, error)) {
      ::close(fd);
      return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    if (!connect_address(fd, reinterpret_cast<const sockaddr*>(&address),
                         sizeof(address), deadline, error)) {
      ::close(fd);
      return -1;
    }
    return fd;
  }

  void close_fd() noexcept {
    if (fd_ >= 0) {
      (void)::close(fd_);
      fd_ = -1;
    }
  }

  int fd_{-1};
  std::size_t buffer_size_{0};
  std::string inbound_;
  std::size_t consume_offset_{0};
};

}  // namespace goblin::core
