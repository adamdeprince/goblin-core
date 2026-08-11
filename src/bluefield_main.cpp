#include "bluefield_relay.hpp"
#include "goblin/core/auth.hpp"
#include "goblin/core/command.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "pubsub.hpp"

#if defined(GOBLIN_HAS_XLIO)
#include "goblin/core/xlio_transport.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sched.h>
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef GOBLIN_CORE_VERSION
#define GOBLIN_CORE_VERSION "development"
#endif

namespace goblin::core::bluefield {
namespace {

using detail::PubSubRegistry;
using detail::PubSubSession;

constexpr std::size_t kDefaultClientBufferBytes = 64U * 1024U;
constexpr std::size_t kDefaultOutputLimitBytes = 16U * 1024U * 1024U;
constexpr std::size_t kReadChunkBytes = 16U * 1024U;

std::atomic_bool g_running{true};

void stop_signal(int) { g_running.store(false, std::memory_order_relaxed); }

[[nodiscard]] bool would_block() noexcept {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

[[nodiscard]] bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_no_sigpipe(int fd) noexcept {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

void set_tcp_nodelay(int fd) noexcept {
  int enabled = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

void set_tcp_quickack(int fd) noexcept {
#ifdef TCP_QUICKACK
  int enabled = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

[[nodiscard]] bool equals_ci(std::string_view lhs,
                             std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto lower = [](unsigned char byte) {
      return byte >= 'A' && byte <= 'Z'
                 ? static_cast<unsigned char>(byte + ('a' - 'A'))
                 : byte;
    };
    if (lower(static_cast<unsigned char>(lhs[index])) !=
        lower(static_cast<unsigned char>(rhs[index]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::uint16_t> parse_port(
    std::string_view text) noexcept {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
      value > std::numeric_limits<std::uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::optional<std::size_t> parse_size(
    std::string_view text) noexcept {
  std::size_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::size_t page_round(std::size_t bytes) {
  const long raw = ::sysconf(_SC_PAGESIZE);
  const std::size_t page =
      raw > 0 ? static_cast<std::size_t>(raw) : std::size_t{4096};
  if (bytes > std::numeric_limits<std::size_t>::max() - (page - 1)) {
    throw std::overflow_error("buffer size is too large");
  }
  return ((bytes + page - 1) / page) * page;
}

struct ResolvedAddress {
  sockaddr_storage storage{};
  socklen_t length{0};
  int family{AF_UNSPEC};
};

[[nodiscard]] ResolvedAddress resolve_tcp(std::string_view host,
                                          std::uint16_t port,
                                          bool passive) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string host_text(host);
  const std::string port_text = std::to_string(port);
  addrinfo* addresses = nullptr;
  const int result = ::getaddrinfo(host_text.empty() ? nullptr : host_text.c_str(),
                                   port_text.c_str(), &hints, &addresses);
  if (result != 0) {
    throw std::runtime_error("resolve " + host_text + ':' + port_text + ": " +
                             ::gai_strerror(result));
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(addresses,
                                                             &::freeaddrinfo);
  if (addresses == nullptr || addresses->ai_addrlen > sizeof(sockaddr_storage)) {
    throw std::runtime_error("no usable address for " + host_text + ':' +
                             port_text);
  }
  ResolvedAddress address;
  std::memcpy(&address.storage, addresses->ai_addr, addresses->ai_addrlen);
  address.length = static_cast<socklen_t>(addresses->ai_addrlen);
  address.family = addresses->ai_family;
  return address;
}

[[nodiscard]] int create_listener(const ResolvedAddress& address, int backlog) {
  int fd = ::socket(address.family, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    throw std::runtime_error("create listener socket: " +
                             std::string(std::strerror(errno)));
  }
  int reuse = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  set_no_sigpipe(fd);
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address.storage),
             address.length) != 0 ||
      ::listen(fd, backlog) != 0 || !set_nonblocking(fd)) {
    const std::string error = std::strerror(errno);
    close_fd(fd);
    throw std::runtime_error("create listener: " + error);
  }
  return fd;
}

[[nodiscard]] std::string read_secret_file(std::string_view path) {
  std::ifstream input(std::string(path), std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open upstream password file '" +
                             std::string(path) + "'");
  }
  std::string value((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  if (value.empty()) {
    throw std::runtime_error("upstream password file is empty");
  }
  return value;
}

void append_resp_command(std::string& out,
                         std::span<const std::string_view> fields) {
  std::size_t bytes = resp::detail::array_header_wire_size(fields.size());
  for (const auto field : fields) {
    const auto field_bytes = resp::detail::bulk_string_wire_size(field.size());
    if (field_bytes > std::numeric_limits<std::size_t>::max() - bytes) {
      throw std::length_error("forwarded RESP command is too large");
    }
    bytes += field_bytes;
  }
  resp::reserve_append_capacity(out, bytes);
  resp::append_array_header(out, fields.size());
  for (const auto field : fields) {
    resp::append_bulk_string(out, field);
  }
}

enum class RespScanStatus : std::uint8_t { incomplete, complete, invalid };

struct RespScanResult {
  RespScanStatus status{RespScanStatus::incomplete};
  std::size_t bytes{0};
};

[[nodiscard]] bool parse_signed_decimal(std::string_view text,
                                        long long& value) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] RespScanResult scan_resp_at(std::string_view bytes,
                                          std::size_t offset,
                                          unsigned depth) noexcept {
  if (offset >= bytes.size()) {
    return {};
  }
  if (depth > 128) {
    return {.status = RespScanStatus::invalid};
  }
  const char type = bytes[offset];
  const auto line_end = bytes.find("\r\n", offset + 1);
  if (line_end == std::string_view::npos) {
    return {};
  }
  const std::size_t after_line = line_end + 2;
  switch (type) {
    case '+':
    case '-':
    case ':':
    case ',':
    case '(':
    case '#':
    case '_':
      return {.status = RespScanStatus::complete, .bytes = after_line - offset};
    case '$':
    case '!':
    case '=': {
      long long length = 0;
      if (!parse_signed_decimal(
              bytes.substr(offset + 1, line_end - offset - 1), length) ||
          length < -1) {
        return {.status = RespScanStatus::invalid};
      }
      if (length == -1) {
        return {.status = RespScanStatus::complete,
                .bytes = after_line - offset};
      }
      const auto payload = static_cast<std::uint64_t>(length);
      if (payload > std::numeric_limits<std::size_t>::max() - after_line - 2) {
        return {.status = RespScanStatus::invalid};
      }
      const std::size_t end = after_line + static_cast<std::size_t>(payload) + 2;
      if (end > bytes.size()) {
        return {};
      }
      if (bytes[end - 2] != '\r' || bytes[end - 1] != '\n') {
        return {.status = RespScanStatus::invalid};
      }
      return {.status = RespScanStatus::complete, .bytes = end - offset};
    }
    case '*':
    case '~':
    case '>':
    case '%':
    case '|': {
      long long count = 0;
      if (!parse_signed_decimal(
              bytes.substr(offset + 1, line_end - offset - 1), count) ||
          count < -1) {
        return {.status = RespScanStatus::invalid};
      }
      if (count == -1) {
        return {.status = RespScanStatus::complete,
                .bytes = after_line - offset};
      }
      std::uint64_t elements = static_cast<std::uint64_t>(count);
      if (type == '%' || type == '|') {
        if (elements > std::numeric_limits<std::uint64_t>::max() / 2) {
          return {.status = RespScanStatus::invalid};
        }
        elements *= 2;
      }
      std::size_t cursor = after_line;
      for (std::uint64_t index = 0; index < elements; ++index) {
        const auto child = scan_resp_at(bytes, cursor, depth + 1);
        if (child.status != RespScanStatus::complete) {
          return child;
        }
        if (child.bytes > bytes.size() - cursor) {
          return {.status = RespScanStatus::invalid};
        }
        cursor += child.bytes;
      }
      return {.status = RespScanStatus::complete, .bytes = cursor - offset};
    }
    default:
      return {.status = RespScanStatus::invalid};
  }
}

[[nodiscard]] RespScanResult scan_resp_frame(std::string_view bytes) noexcept {
  return scan_resp_at(bytes, 0, 0);
}

struct UpstreamCredentials {
  std::string username;
  std::string password;

  [[nodiscard]] bool configured() const noexcept { return !username.empty(); }
};

class ForwardConnection {
 public:
  ForwardConnection(const ResolvedAddress& target,
                    const UpstreamCredentials* credentials)
      : target_(target), credentials_(credentials) {}

  ForwardConnection(const ForwardConnection&) = delete;
  ForwardConnection& operator=(const ForwardConnection&) = delete;
  ForwardConnection(ForwardConnection&&) = delete;
  ForwardConnection& operator=(ForwardConnection&&) = delete;

  ~ForwardConnection() { close_fd(fd_); }

  [[nodiscard]] bool request(std::span<const std::string_view> fields,
                             resp::Version version, std::string& error) {
    if (awaiting_reply_) {
      error = "an upstream reply is already pending";
      return false;
    }
    if (state_ == State::failed) {
      error = error_;
      return false;
    }
    if (state_ == State::closed && !start(error)) {
      return false;
    }

    if (!bootstrapped_) {
      if (credentials_ != nullptr && credentials_->configured()) {
        const std::string_view auth[]{"AUTH", credentials_->username,
                                      credentials_->password};
        append_resp_command(outbound_, auth);
        ++discard_replies_;
      }
      bootstrapped_ = true;
    }
    if (remote_version_ != version) {
      const std::string version_text =
          version == resp::Version::resp3 ? "3" : "2";
      const std::string_view hello[]{"HELLO", version_text};
      append_resp_command(outbound_, hello);
      ++discard_replies_;
      remote_version_ = version;
    }
    append_resp_command(outbound_, fields);
    awaiting_reply_ = true;
    return true;
  }

  [[nodiscard]] std::optional<std::string> poll(std::string& error) {
    if (state_ == State::failed) {
      error = error_;
      return std::nullopt;
    }
    if (state_ == State::connecting) {
      pollfd ready{.fd = fd_, .events = POLLOUT, .revents = 0};
      const int polled = ::poll(&ready, 1, 0);
      if (polled < 0 && errno != EINTR) {
        fail("poll upstream connect: " + std::string(std::strerror(errno)));
      } else if (polled > 0) {
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &length) !=
            0) {
          fail("finish upstream connect: " +
               std::string(std::strerror(errno)));
        } else if (socket_error == 0) {
          state_ = State::active;
        } else if (socket_error != EINPROGRESS && socket_error != EALREADY) {
          fail("upstream connect: " + std::string(std::strerror(socket_error)));
        }
      }
    }
    if (state_ != State::active) {
      if (state_ == State::failed) {
        error = error_;
      }
      return std::nullopt;
    }

    flush();
    if (state_ == State::failed) {
      error = error_;
      return std::nullopt;
    }
    read_ready();
    if (state_ == State::failed) {
      error = error_;
      return std::nullopt;
    }

    for (;;) {
      const auto frame = scan_resp_frame(inbound_);
      if (frame.status == RespScanStatus::incomplete) {
        return std::nullopt;
      }
      if (frame.status == RespScanStatus::invalid) {
        fail("upstream returned a malformed RESP frame");
        error = error_;
        return std::nullopt;
      }
      std::string reply(inbound_.data(), frame.bytes);
      inbound_.erase(0, frame.bytes);
      if (discard_replies_ != 0) {
        --discard_replies_;
        if (!reply.empty() && reply.front() == '-') {
          fail("upstream session setup failed: " + reply.substr(1));
          error = error_;
          return std::nullopt;
        }
        continue;
      }
      awaiting_reply_ = false;
      return reply;
    }
  }

  [[nodiscard]] bool failed() const noexcept { return state_ == State::failed; }

 private:
  enum class State : std::uint8_t { closed, connecting, active, failed };

  [[nodiscard]] bool start(std::string& error) {
    fd_ = ::socket(target_.family, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ < 0 || !set_nonblocking(fd_)) {
      error = "create upstream socket: " + std::string(std::strerror(errno));
      close_fd(fd_);
      return false;
    }
    set_no_sigpipe(fd_);
    set_tcp_nodelay(fd_);
    set_tcp_quickack(fd_);
    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&target_.storage),
                  target_.length) == 0) {
      state_ = State::active;
      return true;
    }
    if (errno == EINPROGRESS) {
      state_ = State::connecting;
      return true;
    }
    error = "connect upstream: " + std::string(std::strerror(errno));
    close_fd(fd_);
    return false;
  }

  void fail(std::string message) {
    error_ = std::move(message);
    state_ = State::failed;
    close_fd(fd_);
  }

  void flush() {
    while (outbound_offset_ < outbound_.size()) {
      const auto remaining = std::string_view(outbound_).substr(outbound_offset_);
      const ssize_t sent =
          ::send(fd_, remaining.data(), remaining.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        outbound_offset_ += static_cast<std::size_t>(sent);
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent < 0 && would_block()) {
        return;
      }
      fail("send upstream request: " + std::string(std::strerror(errno)));
      return;
    }
    if (outbound_offset_ == outbound_.size()) {
      outbound_.clear();
      outbound_offset_ = 0;
    }
  }

  void read_ready() {
    char buffer[kReadChunkBytes];
    for (;;) {
      const ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);
      if (received > 0) {
        inbound_.append(buffer, static_cast<std::size_t>(received));
        continue;
      }
      if (received == 0) {
        fail("upstream RESP connection closed");
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (would_block()) {
        return;
      }
      fail("read upstream reply: " + std::string(std::strerror(errno)));
      return;
    }
  }

  ResolvedAddress target_;
  const UpstreamCredentials* credentials_{nullptr};
  int fd_{-1};
  State state_{State::closed};
  std::string outbound_;
  std::size_t outbound_offset_{0};
  std::string inbound_;
  std::size_t discard_replies_{0};
  resp::Version remote_version_{resp::Version::resp2};
  bool bootstrapped_{false};
  bool awaiting_reply_{false};
  std::string error_;
};

struct ReplyBoundary {
  std::uint64_t sequence{0};
  std::size_t end_offset{0};
};

enum class PendingKind : std::uint8_t { none, upstream, publish };

struct Client : PubSubSession {
  Client(int client_fd, std::uint64_t client_id,
         std::size_t unsolicited_bytes, const ResolvedAddress& upstream,
         const UpstreamCredentials* credentials, bool require_authentication)
      : PubSubSession(unsolicited_bytes),
        fd(client_fd),
        id(client_id),
        forward(upstream, credentials),
        authentication_required(require_authentication),
        authenticated(!require_authentication) {}

#if defined(GOBLIN_HAS_XLIO)
  Client(std::unique_ptr<xlio::Connection> connection,
         std::uint64_t client_id, std::size_t unsolicited_bytes,
         const ResolvedAddress& upstream,
         const UpstreamCredentials* credentials, bool require_authentication)
      : PubSubSession(unsolicited_bytes),
        xlio_connection(std::move(connection)),
        id(client_id),
        forward(upstream, credentials),
        authentication_required(require_authentication),
        authenticated(!require_authentication) {}
#endif

  ~Client() { close_fd(fd); }

  int fd{-1};
#if defined(GOBLIN_HAS_XLIO)
  std::unique_ptr<xlio::Connection> xlio_connection;
#endif
  std::uint64_t id{0};
  ForwardConnection forward;
  RespParser parser;
  std::vector<std::string_view> fields;
  resp::Version resp_version{resp::Version::resp2};
  std::string output;
  std::size_t output_offset{0};
  std::deque<ReplyBoundary> replies;
  std::size_t unsolicited_offset{0};
  bool authentication_required{false};
  bool authenticated{true};
  std::string authenticated_username;
  std::string client_name;
  std::string client_library_name;
  std::string client_library_version;
  bool quit_after_write{false};
  bool dead{false};
  bool in_transaction{false};
  PendingKind pending{PendingKind::none};
  CommandType pending_command{CommandType::unknown};
  std::uint64_t pending_sequence{0};

  [[nodiscard]] std::uint64_t reserve_reply() noexcept {
    return next_output_sequence++;
  }

  void record_reply(std::size_t prior_size) {
    if (output.size() != prior_size) {
      replies.push_back(ReplyBoundary{.sequence = reserve_reply(),
                                      .end_offset = output.size()});
    }
  }

  void append_reserved_reply(std::uint64_t sequence, std::string_view bytes) {
    output.append(bytes);
    replies.push_back(
        ReplyBoundary{.sequence = sequence, .end_offset = output.size()});
  }

  void compact_output() {
    if (replies.empty() && output_offset == output.size()) {
      output.clear();
      output_offset = 0;
    }
  }
};

struct Config {
  std::string listen_address{"127.0.0.1"};
  std::uint16_t listen_port{6379};
  std::string upstream_host;
  std::uint16_t upstream_port{0};
  std::optional<std::string> auth_file;
  UpstreamCredentials upstream_credentials;
  std::size_t unsolicited_bytes{kDefaultClientBufferBytes};
  std::size_t max_output_bytes{kDefaultOutputLimitBytes};
  int backlog{256};
  std::optional<std::uint64_t> edge_id;
  std::optional<unsigned> cpu;
  bool xlio_listener{false};
};

void apply_cpu_affinity(const std::optional<unsigned>& cpu) {
  if (!cpu) {
    return;
  }
#ifdef __linux__
  if (*cpu >= CPU_SETSIZE) {
    throw std::runtime_error("--cpu exceeds the platform CPU set size");
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(*cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    throw std::runtime_error("set BlueField CPU affinity: " +
                             std::string(std::strerror(errno)));
  }
#else
  throw std::runtime_error("--cpu is supported only on Linux");
#endif
}

[[nodiscard]] std::uint64_t make_edge_id() {
  std::random_device random;
  std::uint64_t id = (static_cast<std::uint64_t>(random()) << 32) ^ random() ^
                     static_cast<std::uint64_t>(::getpid()) ^
                     static_cast<std::uint64_t>(
                         std::chrono::steady_clock::now().time_since_epoch().count());
  return id == 0 ? 1 : id;
}

void append_local_error(Client& client, std::string_view message) {
  const std::size_t prior = client.output.size();
  resp::append_error(client.output, message);
  client.record_reply(prior);
}

[[nodiscard]] bool is_pubsub_command(CommandType type) noexcept {
  return type == CommandType::subscribe || type == CommandType::unsubscribe ||
         type == CommandType::psubscribe ||
         type == CommandType::punsubscribe || type == CommandType::publish ||
         type == CommandType::pubsub;
}

[[nodiscard]] bool allowed_while_resp2_subscribed(CommandType type) noexcept {
  return type == CommandType::subscribe || type == CommandType::unsubscribe ||
         type == CommandType::psubscribe ||
         type == CommandType::punsubscribe || type == CommandType::ping ||
         type == CommandType::quit;
}

[[nodiscard]] bool local_connection_command(CommandType type) noexcept {
  return type == CommandType::auth || type == CommandType::hello ||
         type == CommandType::client || type == CommandType::select ||
         type == CommandType::quit || type == CommandType::ping ||
         type == CommandType::echo;
}

[[nodiscard]] CommandType classify_edge_command(
    std::string_view name) noexcept {
  if (name.empty()) {
    return CommandType::unknown;
  }
  const unsigned char first = static_cast<unsigned char>(name.front());
  const char lower = static_cast<char>(
      first >= 'A' && first <= 'Z' ? first + ('a' - 'A') : first);
  switch (lower) {
    case 'a':
      return equals_ci(name, "auth") ? CommandType::auth
                                     : CommandType::unknown;
    case 'c':
      return equals_ci(name, "client") ? CommandType::client
                                       : CommandType::unknown;
    case 'd':
      return equals_ci(name, "discard") ? CommandType::discard
                                        : CommandType::unknown;
    case 'e':
      if (equals_ci(name, "echo")) return CommandType::echo;
      if (equals_ci(name, "exec")) return CommandType::exec;
      return CommandType::unknown;
    case 'h':
      return equals_ci(name, "hello") ? CommandType::hello
                                      : CommandType::unknown;
    case 'm':
      return equals_ci(name, "multi") ? CommandType::multi
                                      : CommandType::unknown;
    case 'p':
      if (equals_ci(name, "publish")) return CommandType::publish;
      if (equals_ci(name, "pubsub")) return CommandType::pubsub;
      if (equals_ci(name, "psubscribe")) return CommandType::psubscribe;
      if (equals_ci(name, "punsubscribe")) return CommandType::punsubscribe;
      if (equals_ci(name, "ping")) return CommandType::ping;
      return CommandType::unknown;
    case 'q':
      return equals_ci(name, "quit") ? CommandType::quit
                                     : CommandType::unknown;
    case 's':
      if (equals_ci(name, "subscribe")) return CommandType::subscribe;
      if (equals_ci(name, "select")) return CommandType::select;
      return CommandType::unknown;
    case 'u':
      if (equals_ci(name, "unsubscribe")) return CommandType::unsubscribe;
      if (equals_ci(name, "unwatch")) return CommandType::unwatch;
      return CommandType::unknown;
    case 'w':
      return equals_ci(name, "watch") ? CommandType::watch
                                      : CommandType::unknown;
    default:
      return CommandType::unknown;
  }
}

[[nodiscard]] std::optional<std::string_view> edge_arity_error(
    CommandType type, std::size_t arguments) noexcept {
  switch (type) {
    case CommandType::ping:
      return arguments <= 1 ? std::nullopt
                            : std::optional<std::string_view>{"ping"};
    case CommandType::auth:
      return arguments >= 1 && arguments <= 2
                 ? std::nullopt
                 : std::optional<std::string_view>{"auth"};
    case CommandType::client:
      return arguments >= 1 ? std::nullopt
                            : std::optional<std::string_view>{"client"};
    case CommandType::select:
      return arguments == 1 ? std::nullopt
                            : std::optional<std::string_view>{"select"};
    case CommandType::quit:
      return arguments == 0 ? std::nullopt
                            : std::optional<std::string_view>{"quit"};
    case CommandType::echo:
      return arguments == 1 ? std::nullopt
                            : std::optional<std::string_view>{"echo"};
    case CommandType::subscribe:
      return arguments >= 1 ? std::nullopt
                            : std::optional<std::string_view>{"subscribe"};
    case CommandType::psubscribe:
      return arguments >= 1 ? std::nullopt
                            : std::optional<std::string_view>{"psubscribe"};
    case CommandType::publish:
      return arguments == 2 ? std::nullopt
                            : std::optional<std::string_view>{"publish"};
    case CommandType::pubsub:
      return arguments >= 1 ? std::nullopt
                            : std::optional<std::string_view>{"pubsub"};
    default:
      return std::nullopt;
  }
}

void append_wrong_arity(Client& client, std::string_view command) {
  std::string message("ERR wrong number of arguments for '");
  message.append(command);
  message.append("' command");
  append_local_error(client, message);
}

void scrub_auth_fields(std::span<const std::string_view> fields) noexcept {
  if (fields.empty() ||
      (!equals_ci(fields.front(), "AUTH") &&
       !equals_ci(fields.front(), "HELLO"))) {
    return;
  }
  for (const auto field : fields.subspan(1)) {
    secure_zero_memory(const_cast<char*>(field.data()), field.size());
  }
}

[[nodiscard]] bool has_pending_output(Client& client) {
  return !client.replies.empty() || !client.unsolicited.empty();
}

struct ClientWriteResult {
  std::size_t consumed{0};
  bool fatal{false};
};

[[nodiscard]] ClientWriteResult try_client_write(
    Client& client, std::string_view bytes) noexcept {
  if (bytes.empty()) {
    return {};
  }
#if defined(GOBLIN_HAS_XLIO)
  if (client.xlio_connection) {
    const std::size_t count =
        std::min(bytes.size(), client.xlio_connection->max_record_payload());
    if (client.xlio_connection->try_push(bytes.substr(0, count))) {
      return {.consumed = count, .fatal = false};
    }
    if (client.xlio_connection->failed() ||
        client.xlio_connection->closed()) {
      return {.consumed = 0, .fatal = true};
    }
    return {};
  }
#endif
  for (;;) {
    const ssize_t sent =
        ::send(client.fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      return {.consumed = static_cast<std::size_t>(sent), .fatal = false};
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent < 0 && would_block()) {
      return {};
    }
    return {.consumed = 0, .fatal = true};
  }
}

[[nodiscard]] bool write_client(Client& client) {
  while (has_pending_output(client)) {
    const auto push = client.unsolicited.front();
    const bool write_push =
        push && (client.replies.empty() ||
                 push->sequence < client.replies.front().sequence);
    std::string_view bytes;
    if (write_push) {
      bytes = push->bytes.substr(client.unsolicited_offset);
    } else {
      const auto end = client.replies.front().end_offset;
      bytes = std::string_view(client.output).substr(client.output_offset,
                                                     end - client.output_offset);
    }
    const auto result = try_client_write(client, bytes);
    if (result.consumed != 0) {
      const auto count = result.consumed;
      if (write_push) {
        client.unsolicited_offset += count;
        if (client.unsolicited_offset == push->bytes.size()) {
          client.unsolicited.pop_front(push->bytes.size());
          client.unsolicited_offset = 0;
        }
      } else {
        client.output_offset += count;
        if (client.output_offset == client.replies.front().end_offset) {
          client.replies.pop_front();
          client.compact_output();
        }
      }
      continue;
    }
    return !result.fatal;
  }
  return true;
}

[[nodiscard]] detail::PubSubDirectWriteResult try_direct_pubsub_write(
    void*, PubSubSession& session, std::string_view bytes) noexcept {
  auto& client = static_cast<Client&>(session);
  // A queued command reply may have an earlier sequence number. In that case
  // the normal unsolicited ring performs the merge in write_client().
  if (client.dead || !client.replies.empty()) {
    return {};
  }
  const auto result = try_client_write(client, bytes);
  if (result.fatal) {
    client.dead = true;
    return {.consumed = 0, .fatal = true};
  }
  return {.consumed = result.consumed, .fatal = false};
}

[[nodiscard]] bool valid_client_token(std::string_view value,
                                      bool allow_empty = true) noexcept {
  if (value.empty()) {
    return allow_empty;
  }
  if (value.size() > 256) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e;
  });
}

[[nodiscard]] bool authenticate(Client& client, const AuthDatabase* auth,
                                std::string_view username,
                                std::string_view password) {
  if (auth == nullptr) {
    resp::append_error(
        client.output,
        "ERR AUTH called without any password configured for this server");
    return false;
  }
  if (!auth->verify(username, password)) {
    resp::append_error(
        client.output,
        "WRONGPASS invalid username-password pair or user is disabled.");
    return false;
  }
  client.authenticated = true;
  client.authenticated_username.assign(username);
  return true;
}

void append_hello_response(Client& client, resp::Version version) {
  constexpr std::size_t kFieldCount = 7;
  if (version == resp::Version::resp3) {
    resp::append_map_header(client.output, kFieldCount);
  } else {
    resp::append_array_header(client.output, kFieldCount * 2);
  }
  const auto append_string = [&client](std::string_view key,
                                       std::string_view value) {
    resp::append_bulk_string(client.output, key);
    resp::append_bulk_string(client.output, value);
  };
  append_string("server", "goblin-core-bluefield");
  append_string("version", GOBLIN_CORE_VERSION);
  resp::append_bulk_string(client.output, "proto");
  resp::append_integer(client.output, static_cast<unsigned>(version));
  resp::append_bulk_string(client.output, "id");
  resp::append_integer(client.output, static_cast<long long>(client.id));
  append_string("mode", "standalone");
  append_string("role", "master");
  resp::append_bulk_string(client.output, "modules");
  resp::append_array_header(client.output, 0);
}

void execute_local_connection_command(Client& client, const AuthDatabase* auth,
                                      const Command& command) {
  const std::size_t prior = client.output.size();
  switch (command.type) {
    case CommandType::ping:
      if (command.args.empty()) {
        resp::append_simple_string(client.output, "PONG");
      } else {
        resp::append_bulk_string(client.output, command.args.front());
      }
      break;
    case CommandType::echo:
      resp::append_bulk_string(client.output, command.args.front());
      break;
    case CommandType::auth: {
      const auto username = command.args.size() == 1
                                ? std::string_view{"default"}
                                : command.args.front();
      if (authenticate(client, auth, username, command.args.back())) {
        resp::append_simple_string(client.output, "OK");
      }
      break;
    }
    case CommandType::hello: {
      auto selected = client.resp_version;
      if (command.args.empty()) {
        append_hello_response(client, selected);
        break;
      }
      long long requested = 0;
      if (!parse_signed_decimal(command.args.front(), requested) ||
          (requested != 2 && requested != 3)) {
        resp::append_error(client.output,
                           "NOPROTO unsupported protocol version");
        break;
      }
      selected = requested == 3 ? resp::Version::resp3 : resp::Version::resp2;
      bool saw_auth = false;
      bool saw_setname = false;
      std::string_view username;
      std::string_view password;
      std::string_view requested_name;
      std::size_t index = 1;
      bool syntax_ok = true;
      while (index < command.args.size()) {
        if (!saw_auth && equals_ci(command.args[index], "AUTH") &&
            index + 2 < command.args.size()) {
          saw_auth = true;
          username = command.args[index + 1];
          password = command.args[index + 2];
          index += 3;
        } else if (!saw_setname &&
                   equals_ci(command.args[index], "SETNAME") &&
                   index + 1 < command.args.size()) {
          saw_setname = true;
          requested_name = command.args[index + 1];
          index += 2;
        } else {
          syntax_ok = false;
          break;
        }
      }
      if (!syntax_ok) {
        resp::append_error(client.output, "ERR syntax error");
      } else if (saw_setname && !valid_client_token(requested_name)) {
        resp::append_error(
            client.output,
            "ERR Client names cannot contain spaces, newlines or special characters.");
      } else if (!saw_auth || authenticate(client, auth, username, password)) {
        client.resp_version = selected;
        if (saw_setname) client.client_name.assign(requested_name);
        append_hello_response(client, selected);
      }
      break;
    }
    case CommandType::client: {
      const auto subcommand = command.args.front();
      if (equals_ci(subcommand, "SETNAME")) {
        if (command.args.size() != 2) {
          std::string message(
              "ERR wrong number of arguments for 'client|setname' command");
          resp::append_error(client.output, message);
        } else if (!valid_client_token(command.args[1])) {
          resp::append_error(
              client.output,
              "ERR Client names cannot contain spaces, newlines or special characters.");
        } else {
          client.client_name.assign(command.args[1]);
          resp::append_simple_string(client.output, "OK");
        }
      } else if (equals_ci(subcommand, "GETNAME")) {
        if (command.args.size() != 1) {
          resp::append_error(
              client.output,
              "ERR wrong number of arguments for 'client|getname' command");
        } else if (client.client_name.empty()) {
          resp::append_null(client.output, client.resp_version);
        } else {
          resp::append_bulk_string(client.output, client.client_name);
        }
      } else if (equals_ci(subcommand, "ID")) {
        if (command.args.size() != 1) {
          resp::append_error(
              client.output,
              "ERR wrong number of arguments for 'client|id' command");
        } else {
          resp::append_integer(client.output, static_cast<long long>(client.id));
        }
      } else if (equals_ci(subcommand, "SETINFO")) {
        if (command.args.size() != 3) {
          resp::append_error(
              client.output,
              "ERR wrong number of arguments for 'client|setinfo' command");
        } else if (!valid_client_token(command.args[2], false)) {
          resp::append_error(client.output,
                             "ERR invalid client library information");
        } else if (equals_ci(command.args[1], "LIB-NAME")) {
          client.client_library_name.assign(command.args[2]);
          resp::append_simple_string(client.output, "OK");
        } else if (equals_ci(command.args[1], "LIB-VER")) {
          client.client_library_version.assign(command.args[2]);
          resp::append_simple_string(client.output, "OK");
        } else {
          resp::append_error(client.output,
                             "ERR unknown CLIENT SETINFO option");
        }
      } else {
        resp::append_error(client.output, "ERR unknown subcommand for CLIENT");
      }
      break;
    }
    case CommandType::select: {
      long long database = 0;
      if (!parse_signed_decimal(command.args.front(), database)) {
        resp::append_error(client.output,
                           "ERR value is not an integer or out of range");
      } else if (database != 0) {
        resp::append_error(client.output, "ERR DB index is out of range");
      } else {
        resp::append_simple_string(client.output, "OK");
      }
      break;
    }
    case CommandType::quit:
      resp::append_simple_string(client.output, "OK");
      client.quit_after_write = true;
      break;
    default:
      break;
  }
  client.record_reply(prior);
}

void dispatch_command(Client& client, PubSubRegistry& pubsub, EdgeRelay& relay,
                      const AuthDatabase* auth,
                      std::span<const std::string_view> fields) {
  if (fields.empty()) {
    append_local_error(client, "ERR Protocol error: empty command");
    return;
  }
  const auto type = classify_edge_command(fields.front());
  const auto arguments = fields.subspan(1);
  if (const auto arity = edge_arity_error(type, arguments.size())) {
    append_wrong_arity(client, *arity);
    scrub_auth_fields(fields);
    return;
  }
  const Command command{
      .type = type, .name = fields.front(), .args = arguments};
  if (client.authentication_required && !client.authenticated &&
      command.type != CommandType::auth && command.type != CommandType::hello &&
      command.type != CommandType::quit) {
    append_local_error(client, "NOAUTH Authentication required.");
    scrub_auth_fields(fields);
    return;
  }
  if (client.wire_mode == detail::WireMode::resp2 &&
      client.subscription_count() != 0 &&
      !allowed_while_resp2_subscribed(command.type)) {
    append_local_error(
        client, "ERR Can't execute this command while subscribed to a channel");
    scrub_auth_fields(fields);
    return;
  }
  if (client.wire_mode == detail::WireMode::resp2 &&
      client.subscription_count() != 0 && command.type == CommandType::ping) {
    const std::size_t prior = client.output.size();
    resp::append_array_header(client.output, 2);
    resp::append_bulk_string(client.output, "pong");
    resp::append_bulk_string(
        client.output,
        command.args.empty() ? std::string_view{} : command.args.front());
    client.record_reply(prior);
  } else if (!client.in_transaction && is_pubsub_command(command.type)) {
    if (command.type == CommandType::publish) {
      const long long local = pubsub.publish(command.args[0], command.args[1]);
      const auto sequence = client.reserve_reply();
      relay.stage_publish(client.id, sequence, local, command.args[0],
                          command.args[1]);
      client.pending = PendingKind::publish;
      client.pending_command = command.type;
      client.pending_sequence = sequence;
    } else {
      const std::size_t prior = client.output.size();
      pubsub.execute(client, command, client.output);
      client.record_reply(prior);
    }
  } else if (!client.in_transaction &&
             local_connection_command(command.type)) {
    execute_local_connection_command(client, auth, command);
  } else {
    std::string error;
    const auto sequence = client.reserve_reply();
    if (!client.forward.request(fields, client.resp_version, error)) {
      client.append_reserved_reply(
          sequence, "-ERR BlueField upstream unavailable: " + error + "\r\n");
    } else {
      client.pending = PendingKind::upstream;
      client.pending_command = command.type;
      client.pending_sequence = sequence;
    }
  }
  client.wire_mode = client.resp_version == resp::Version::resp3
                         ? detail::WireMode::resp3
                         : detail::WireMode::resp2;
  scrub_auth_fields(fields);
}

[[nodiscard]] bool process_buffered(Client& client, PubSubRegistry& pubsub,
                                    EdgeRelay& relay,
                                    const AuthDatabase* auth,
                                    std::size_t max_output_bytes) {
  while (client.pending == PendingKind::none &&
         client.parser.pop_into(client.fields)) {
    dispatch_command(client, pubsub, relay, auth, client.fields);
    if (client.output.size() > max_output_bytes || client.close_requested) {
      return false;
    }
    if (client.quit_after_write) {
      break;
    }
  }
  if (client.pending == PendingKind::none && client.parser.has_error()) {
    append_local_error(client, client.parser.error());
    client.quit_after_write = true;
  }
  return true;
}

[[nodiscard]] bool read_client(Client& client, PubSubRegistry& pubsub,
                               EdgeRelay& relay,
                               const AuthDatabase* auth,
                               std::size_t max_output_bytes) {
#if defined(GOBLIN_HAS_XLIO)
  if (client.xlio_connection) {
    while (const auto received = client.xlio_connection->peek()) {
      client.parser.append(*received);
      client.xlio_connection->pop();
      if (!process_buffered(client, pubsub, relay, auth, max_output_bytes)) {
        return false;
      }
      if (client.pending != PendingKind::none || client.quit_after_write) {
        return true;
      }
    }
    return !client.xlio_connection->failed() &&
           !client.xlio_connection->closed();
  }
#endif
  char buffer[kReadChunkBytes];
  for (;;) {
    const ssize_t received = ::recv(client.fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      client.parser.append(
          std::string_view(buffer, static_cast<std::size_t>(received)));
      if (!process_buffered(client, pubsub, relay, auth, max_output_bytes)) {
        return false;
      }
      if (client.pending != PendingKind::none || client.quit_after_write) {
        return true;
      }
      continue;
    }
    if (received == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return would_block();
  }
}

void finish_upstream_reply(Client& client, std::string reply) {
  client.append_reserved_reply(client.pending_sequence, reply);
  const bool error = !reply.empty() && reply.front() == '-';
  if (!error && client.pending_command == CommandType::multi) {
    client.in_transaction = true;
  }
  if (client.pending_command == CommandType::exec ||
      client.pending_command == CommandType::discard) {
    client.in_transaction = false;
  }
  client.pending = PendingKind::none;
  client.pending_command = CommandType::unknown;
  client.pending_sequence = 0;
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
  Config config;
  std::optional<std::string> upstream_password_file;
  bool listener_configured = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      std::cout
          << "usage: goblin-core-bluefield --upstream HOST PORT [options]\n"
          << "  --listen ADDRESS PORT\n"
#if defined(GOBLIN_HAS_XLIO)
          << "  --xlio ADDRESS PORT\n"
#endif
          << "  --auth-file PATH\n"
          << "  --upstream-auth-user USER\n"
          << "  --upstream-auth-password-file PATH\n"
          << "  --unsolicited-output-buffer-bytes BYTES\n"
          << "  --max-output-buffer-bytes BYTES\n"
          << "  --edge-id NONZERO-U64\n"
          << "  --cpu N\n"
          << "  --backlog N\n";
      std::exit(0);
    }
    if (arg == "--listen" && index + 2 < argc) {
      if (listener_configured) {
        throw std::runtime_error("configure exactly one client listener");
      }
      config.listen_address = argv[++index];
      const auto port = parse_port(argv[++index]);
      if (!port) {
        throw std::runtime_error("invalid --listen port");
      }
      config.listen_port = *port;
      config.xlio_listener = false;
      listener_configured = true;
      continue;
    }
    if (arg == "--xlio" && index + 2 < argc) {
#if defined(GOBLIN_HAS_XLIO)
      if (listener_configured) {
        throw std::runtime_error("configure exactly one client listener");
      }
      config.listen_address = argv[++index];
      const auto port = parse_port(argv[++index]);
      if (!port) {
        throw std::runtime_error("invalid --xlio port");
      }
      config.listen_port = *port;
      config.xlio_listener = true;
      listener_configured = true;
      continue;
#else
      throw std::runtime_error(
          "--xlio requires a GOBLIN_BLUEFIELD_ENABLE_XLIO build");
#endif
    }
    if (arg == "--upstream" && index + 2 < argc) {
      config.upstream_host = argv[++index];
      const auto port = parse_port(argv[++index]);
      if (!port) {
        throw std::runtime_error("invalid --upstream port");
      }
      config.upstream_port = *port;
      continue;
    }
    if (arg == "--auth-file" && index + 1 < argc) {
      config.auth_file = argv[++index];
      continue;
    }
    if (arg == "--upstream-auth-user" && index + 1 < argc) {
      config.upstream_credentials.username = argv[++index];
      continue;
    }
    if (arg == "--upstream-auth-password-file" && index + 1 < argc) {
      upstream_password_file = argv[++index];
      continue;
    }
    if (arg == "--unsolicited-output-buffer-bytes" && index + 1 < argc) {
      const auto bytes = parse_size(argv[++index]);
      if (!bytes) {
        throw std::runtime_error(
            "invalid --unsolicited-output-buffer-bytes value");
      }
      config.unsolicited_bytes = *bytes;
      continue;
    }
    if (arg == "--max-output-buffer-bytes" && index + 1 < argc) {
      const auto bytes = parse_size(argv[++index]);
      if (!bytes) {
        throw std::runtime_error("invalid --max-output-buffer-bytes value");
      }
      config.max_output_bytes = *bytes;
      continue;
    }
    if (arg == "--edge-id" && index + 1 < argc) {
      std::uint64_t value = 0;
      const std::string_view text(argv[++index]);
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() ||
          value == 0) {
        throw std::runtime_error("invalid --edge-id value");
      }
      config.edge_id = value;
      continue;
    }
    if (arg == "--cpu" && index + 1 < argc) {
      unsigned value = 0;
      const std::string_view text(argv[++index]);
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("invalid --cpu value");
      }
      config.cpu = value;
      continue;
    }
    if (arg == "--backlog" && index + 1 < argc) {
      unsigned value = 0;
      const std::string_view text(argv[++index]);
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() ||
          value == 0 || value > std::numeric_limits<int>::max()) {
        throw std::runtime_error("invalid --backlog value");
      }
      config.backlog = static_cast<int>(value);
      continue;
    }
    throw std::runtime_error("unknown or incomplete option: " +
                             std::string(arg));
  }
  if (config.upstream_host.empty() || config.upstream_port == 0) {
    throw std::runtime_error("--upstream HOST PORT is required");
  }
  if (config.upstream_credentials.username.empty() !=
      !upstream_password_file.has_value()) {
    throw std::runtime_error(
        "--upstream-auth-user and --upstream-auth-password-file must be used together");
  }
  if (upstream_password_file) {
    config.upstream_credentials.password =
        read_secret_file(*upstream_password_file);
  }
  config.unsolicited_bytes = page_round(config.unsolicited_bytes);
  return config;
}

int run(const Config& config) {
  apply_cpu_affinity(config.cpu);
  const auto upstream =
      resolve_tcp(config.upstream_host, config.upstream_port, false);
  int listener = -1;
#if defined(GOBLIN_HAS_XLIO)
  std::unique_ptr<xlio::ServerListener> xlio_listener;
  if (config.xlio_listener) {
    std::string error;
    xlio_listener = xlio::ServerListener::create(
        config.listen_address, config.listen_port, error);
    if (!xlio_listener) {
      throw std::runtime_error("create XLIO Ultra listener: " + error);
    }
  } else
#endif
  {
    const auto listen =
        resolve_tcp(config.listen_address, config.listen_port, true);
    listener = create_listener(listen, config.backlog);
  }

  std::optional<AuthDatabase> auth;
  if (config.auth_file) {
    auth.emplace(AuthDatabase::load(*config.auth_file));
  }
  PubSubRegistry pubsub;
  pubsub.set_direct_writer(
      {.context = nullptr, .write = &try_direct_pubsub_write});
  const std::uint64_t edge_id = config.edge_id.value_or(make_edge_id());
  EdgeRelay relay(config.upstream_host, config.upstream_port, edge_id);
  pubsub.set_subscription_observer(relay.observer());

  std::vector<std::unique_ptr<Client>> clients;
  ankerl::unordered_dense::map<std::uint64_t, Client*> clients_by_id;
  std::uint64_t next_client_id = 1;
  std::vector<PublishCompletion> completions;
  clients.reserve(static_cast<std::size_t>(std::max(config.backlog, 1)));
  clients_by_id.reserve(static_cast<std::size_t>(std::max(config.backlog, 1)));
  completions.reserve(64);

  std::cout << "goblin-core-bluefield listening via "
            << (config.xlio_listener ? "XLIO Ultra" : "kernel TCP") << " on "
            << config.listen_address << ':' << config.listen_port << ", upstream "
            << config.upstream_host << ':' << config.upstream_port
            << ", edge id " << edge_id << '\n';

  while (g_running.load(std::memory_order_relaxed)) {
    bool progressed = false;
#if defined(GOBLIN_HAS_XLIO)
    if (xlio_listener) {
      auto event = xlio_listener->poll();
      progressed = event.progressed;
      if (!xlio_listener->error().empty()) {
        throw std::runtime_error("XLIO Ultra listener: " +
                                 std::string(xlio_listener->error()));
      }
      if (event.connection) {
        const auto client_id = next_client_id++;
        auto client = std::make_unique<Client>(
            std::move(event.connection), client_id, config.unsolicited_bytes,
            upstream,
            config.upstream_credentials.configured()
                ? &config.upstream_credentials
                : nullptr,
            auth.has_value());
        clients_by_id.emplace(client_id, client.get());
        clients.push_back(std::move(client));
        progressed = true;
      }
    } else
#endif
    {
    for (;;) {
      sockaddr_storage peer{};
      socklen_t peer_length = sizeof(peer);
      int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&peer),
                        &peer_length);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (would_block()) {
          break;
        }
        throw std::runtime_error("accept: " +
                                 std::string(std::strerror(errno)));
      }
      if (!set_nonblocking(fd)) {
        close_fd(fd);
        continue;
      }
      set_no_sigpipe(fd);
      set_tcp_nodelay(fd);
      set_tcp_quickack(fd);
      const auto client_id = next_client_id++;
      auto client = std::make_unique<Client>(
          fd, client_id, config.unsolicited_bytes, upstream,
          config.upstream_credentials.configured()
              ? &config.upstream_credentials
              : nullptr,
          auth.has_value());
      clients_by_id.emplace(client_id, client.get());
      clients.push_back(std::move(client));
      progressed = true;
    }
    }

    completions.clear();
    if (relay.poll(pubsub, completions)) {
      progressed = true;
    }
    for (const auto& completion : completions) {
      const auto found = clients_by_id.find(completion.client_id);
      if (found == clients_by_id.end()) {
        continue;
      }
      auto& client = *found->second;
      if (client.pending != PendingKind::publish ||
          client.pending_sequence != completion.reply_sequence) {
        client.dead = true;
        continue;
      }
      std::string reply;
      resp::append_integer(reply, completion.local_deliveries +
                                      completion.upstream_deliveries);
      client.append_reserved_reply(completion.reply_sequence, reply);
      client.pending = PendingKind::none;
      client.pending_command = CommandType::unknown;
      client.pending_sequence = 0;
    }

    for (auto& client_ptr : clients) {
      auto& client = *client_ptr;
      if (client.dead) {
        continue;
      }
#if defined(GOBLIN_HAS_XLIO)
      if (client.xlio_connection &&
          (client.xlio_connection->failed() ||
           client.xlio_connection->closed())) {
        client.dead = true;
        continue;
      }
#endif
      if (client.pending == PendingKind::upstream) {
        std::string error;
        if (auto reply = client.forward.poll(error)) {
          finish_upstream_reply(client, std::move(*reply));
          progressed = true;
        } else if (client.forward.failed()) {
          client.append_reserved_reply(
              client.pending_sequence,
              "-ERR BlueField upstream failure: " + error + "\r\n");
          client.pending = PendingKind::none;
          client.quit_after_write = true;
          progressed = true;
        }
      }

      if (client.pending == PendingKind::none && !client.quit_after_write &&
          !process_buffered(client, pubsub, relay, auth ? &*auth : nullptr,
                            config.max_output_bytes)) {
        client.dead = true;
      }
      if (!client.dead && client.pending == PendingKind::none &&
          !client.quit_after_write &&
          !read_client(client, pubsub, relay, auth ? &*auth : nullptr,
                       config.max_output_bytes)) {
        client.dead = true;
      }
      if (!client.dead && has_pending_output(client)) {
        if (!write_client(client)) {
          client.dead = true;
        } else {
          progressed = true;
        }
      }
      if (!client.dead &&
          (client.close_requested ||
           (client.quit_after_write && !has_pending_output(client)))) {
        client.dead = true;
      }
    }

    // A local PUBLISH queues subscriber pushes before it is staged on the SBE
    // publisher link. Give every local socket a non-blocking flush first; a
    // backpressured subscriber is attempted but cannot hold up the host fanout.
    if (relay.has_staged()) {
      for (auto& client_ptr : clients) {
        auto& client = *client_ptr;
        if (!client.dead && has_pending_output(client) &&
            !write_client(client)) {
          client.dead = true;
        }
      }
      if (relay.flush_staged()) {
        progressed = true;
      }
    }

    for (std::size_t index = clients.size(); index > 0; --index) {
      auto& client = *clients[index - 1];
      if (!client.dead) {
        continue;
      }
      relay.cancel(client.id);
      pubsub.remove(client);
      clients_by_id.erase(client.id);
      clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index - 1));
      progressed = true;
    }

    if (!progressed) {
      cpu_relax();
    }
  }

  pubsub.set_subscription_observer({});
  for (auto& client : clients) {
    pubsub.remove(*client);
  }
  clients.clear();
  close_fd(listener);
  return 0;
}

}  // namespace
}  // namespace goblin::core::bluefield

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, goblin::core::bluefield::stop_signal);
  std::signal(SIGTERM, goblin::core::bluefield::stop_signal);
  try {
    return goblin::core::bluefield::run(
        goblin::core::bluefield::parse_args(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "goblin-core-bluefield: " << error.what() << '\n';
    return 1;
  }
}
