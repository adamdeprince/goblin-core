#pragma once

// TCP transport adapter for BasicSbeClient / RESP clients, optionally accelerated
// by Cisco ExaSock when the process is launched under `exasock` and the peer is
// reached via an ExaNIC interface.
//
// Compile-time: GOBLIN_HAS_EXASOCK (from -DGOBLIN_CORE_ENABLE_EXASOCK=ON).
// Runtime: acceleration only applies under the exasock preload when the route
// uses an ExaNIC; otherwise this is an ordinary non-blocking TCP client.

#if defined(GOBLIN_HAS_EXASOCK)

#include "goblin/core/exasock.hpp"
#include "goblin/core/ring_buffer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace goblin::core::exasock {

// Mirrors rdma::ClientTransport / RingSbeTransport so BasicSbeClient can reuse
// one typed command path over accelerated TCP.
class ClientTransport {
 public:
  using ms = std::chrono::milliseconds;

  ClientTransport(const ClientTransport&) = delete;
  ClientTransport& operator=(const ClientTransport&) = delete;
  ClientTransport(ClientTransport&&) noexcept = default;
  ClientTransport& operator=(ClientTransport&&) noexcept = default;
  ~ClientTransport() { close_fd(); }

  [[nodiscard]] static std::optional<ClientTransport> open(
      std::string_view host, std::uint16_t port,
      ms timeout = ms(5000), std::size_t buffer_size = 64 * 1024,
      ConnectOptions options = {}, std::string* error = nullptr) {
    if (options.require_loaded && !loaded()) {
      if (error != nullptr) {
        *error =
            "ExaSock is not loaded; run the client under the exasock wrapper "
            "(exasock <binary> ...)";
      }
      return std::nullopt;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    const std::string host_storage(host);
    const std::string port_storage = std::to_string(port);
    const int gai = ::getaddrinfo(host_storage.c_str(), port_storage.c_str(),
                                  &hints, &result);
    if (gai != 0) {
      if (error != nullptr) {
        *error = std::string("getaddrinfo: ") + ::gai_strerror(gai);
      }
      return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int fd = -1;
    std::string last_error = "connect failed";
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
      fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) {
        last_error = std::string("socket: ") + std::strerror(errno);
        continue;
      }
      int one = 1;
      (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      const int flags = ::fcntl(fd, F_GETFL, 0);
      if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }

      int rc = 0;
#if defined(GOBLIN_HAS_EXASOCK)
      if (options.ate_id >= 0 && loaded()) {
        rc = ::exasock_ate_connect(fd, options.ate_id, ai->ai_addr,
                                  static_cast<socklen_t>(ai->ai_addrlen));
      } else {
        rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
      }
#else
      rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
#endif
      if (rc == 0) {
        break;
      }
      if (errno != EINPROGRESS) {
        last_error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
        continue;
      }

      for (;;) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          last_error = "connect timed out";
          ::close(fd);
          fd = -1;
          break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const int pr = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (pr < 0) {
          if (errno == EINTR) {
            continue;
          }
          last_error = std::string("poll: ") + std::strerror(errno);
          ::close(fd);
          fd = -1;
          break;
        }
        if (pr == 0) {
          last_error = "connect timed out";
          ::close(fd);
          fd = -1;
          break;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
          last_error = std::string("getsockopt: ") + std::strerror(errno);
          ::close(fd);
          fd = -1;
          break;
        }
        if (so_error != 0) {
          last_error = std::string("connect: ") + std::strerror(so_error);
          ::close(fd);
          fd = -1;
          break;
        }
        // Connected.
        goto connected;
      }
      if (fd < 0) {
        continue;
      }
    connected:
      break;
    }
    ::freeaddrinfo(result);

    if (fd < 0) {
      if (error != nullptr) {
        *error = std::move(last_error);
      }
      return std::nullopt;
    }

    return ClientTransport(fd, buffer_size, options.ate_id);
  }

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) noexcept {
    // Warm the accelerated TX path with a same-sized payload when available.
    if (warm_enabled_ && !bytes.empty()) {
      warm_send(fd_, bytes);
    }
    while (!bytes.empty()) {
      const ssize_t n =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (n > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EINTR)) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (stop()) {
          return false;
        }
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLOUT;
        (void)::poll(&pfd, 1, 0);
        ring::cpu_relax();
        continue;
      }
      failed_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string_view> peek() noexcept {
    if (consume_offset_ < inbound_.size()) {
      return std::string_view(inbound_.data() + consume_offset_,
                              inbound_.size() - consume_offset_);
    }
    if (!recv_some()) {
      return std::nullopt;
    }
    if (consume_offset_ < inbound_.size()) {
      return std::string_view(inbound_.data() + consume_offset_,
                              inbound_.size() - consume_offset_);
    }
    return std::nullopt;
  }

  void pop() noexcept {
    // The SBE client appends the full peek view then pops; drain the buffer.
    consume_offset_ = inbound_.size();
    if (consume_offset_ > 64 * 1024) {
      inbound_.clear();
      consume_offset_ = 0;
    }
  }

  // Consume exactly `n` bytes previously returned from peek() (for stream
  // protocols that parse partial frames). Prefer pop() for ring-style full-record
  // consumption.
  void consume(std::size_t n) noexcept {
    consume_offset_ += n;
    if (consume_offset_ >= inbound_.size()) {
      inbound_.clear();
      consume_offset_ = 0;
    } else if (consume_offset_ > 64 * 1024) {
      inbound_.erase(0, consume_offset_);
      consume_offset_ = 0;
    }
  }

  void wait_for_record() const noexcept { ring::cpu_relax(); }

  [[nodiscard]] std::size_t send_capacity() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] std::size_t receive_capacity() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] std::size_t max_message_bytes() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] std::size_t buffer_size_hint() const noexcept {
    return buffer_size_;
  }
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] bool accelerated() const noexcept {
    std::string device;
    int port = -1;
    return tcp_device(fd_, device, port);
  }
  [[nodiscard]] bool warm_enabled() const noexcept { return warm_enabled_; }
  void set_warm_enabled(bool enabled) noexcept {
    warm_enabled_ = enabled && supports_frame_warm();
  }

 private:
  ClientTransport(int fd, std::size_t buffer_size, int ate_id) noexcept
      : fd_(fd),
        buffer_size_(buffer_size == 0 ? std::size_t{64} << 10 : buffer_size),
        ate_id_(ate_id),
        warm_enabled_(supports_frame_warm()) {
    inbound_.reserve(buffer_size_);
  }

  void close_fd() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  [[nodiscard]] bool recv_some() noexcept {
    if (failed_ || fd_ < 0) {
      return false;
    }
    char chunk[16384];
    const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (n > 0) {
      if (consume_offset_ > 0 && consume_offset_ == inbound_.size()) {
        inbound_.clear();
        consume_offset_ = 0;
      }
      inbound_.append(chunk, static_cast<std::size_t>(n));
      return true;
    }
    if (n == 0) {
      failed_ = true;
      return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return false;
    }
    failed_ = true;
    return false;
  }

  int fd_{-1};
  std::size_t buffer_size_{0};
  int ate_id_{-1};
  bool warm_enabled_{false};
  bool failed_{false};
  std::string inbound_;
  std::size_t consume_offset_{0};
};

}  // namespace goblin::core::exasock

#endif  // GOBLIN_HAS_EXASOCK
