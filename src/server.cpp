#include "goblin/core/server.hpp"

#include "goblin/core/auth.hpp"
#include "goblin/core/command.hpp"
#include "goblin/core/goblin_protocol.hpp"
#ifdef GOBLIN_HAS_KAFKA
#include "goblin/core/kafka_ingest.hpp"
#endif
#include "goblin/core/luau_script.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/ring_buffer.hpp"
#ifdef GOBLIN_HAS_RDMA
#include "goblin/core/rdma_ring.hpp"
#endif
#include "goblin/core/exasock.hpp"
#include "goblin/core/script.hpp"
#ifdef GOBLIN_HAS_SBE
#include "goblin/core/sbe_dispatch.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/PSubscribe.h"
#include "goblin_sbe/PUnsubscribe.h"
#include "goblin_sbe/PubSub.h"
#include "goblin_sbe/Publish.h"
#include "goblin_sbe/Subscribe.h"
#include "goblin_sbe/Unsubscribe.h"
#endif
#include "goblin/core/store.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/wren_script.hpp"
#include "pubsub.hpp"
#include "transaction.hpp"
#ifdef GOBLIN_HAS_SBE
#include "pubsub_listener.hpp"
#endif

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace goblin::core {
namespace {

constexpr std::size_t kOutputCompactThreshold = 64 * 1024;

struct ReplyBoundary {
  std::uint64_t sequence{0};
  std::size_t end_offset{0};
};

struct EndpointSession : detail::PubSubSession {
  EndpointSession(std::size_t unsolicited_output_bytes,
                  std::size_t transaction_buffer_bytes,
                  std::uint64_t assigned_connection_id,
                  bool require_authentication)
      : PubSubSession(unsolicited_output_bytes),
        transaction(transaction_buffer_bytes),
        connection_id(assigned_connection_id),
        default_authentication_required(require_authentication),
        authentication_required(require_authentication),
        authenticated(!require_authentication) {}

  detail::TransactionState transaction;
  resp::Version resp_version{resp::Version::resp2};
  std::uint64_t connection_id{0};
  bool default_authentication_required{false};
  bool authentication_required{false};
  bool authenticated{true};
  std::string authenticated_username;
  std::string client_name;
  std::string client_library_name;
  std::string client_library_version;
  bool quit_after_write{false};
  RespParser parser;
  std::string inbuf;   // raw accumulator: pre-decision bytes, then the SBE stream
  std::string output;
  std::vector<std::string_view> fields;
  std::size_t output_offset{0};
  std::vector<ReplyBoundary> replies;
  std::size_t reply_index{0};

  void record_reply(std::size_t prior_size) {
    if (output.size() != prior_size) {
      replies.push_back(ReplyBoundary{.sequence = next_output_sequence++,
                                      .end_offset = output.size()});
    }
  }

  void reset_connection(std::uint64_t assigned_connection_id) {
    wire_mode = detail::WireMode::undecided;
    resp_version = resp::Version::resp2;
    connection_id = assigned_connection_id;
    authentication_required = default_authentication_required;
    authenticated = !authentication_required;
    authenticated_username.clear();
    client_name.clear();
    client_library_name.clear();
    client_library_version.clear();
    quit_after_write = false;
    parser.clear();
    inbuf.clear();
    output.clear();
    output_offset = 0;
    replies.clear();
    reply_index = 0;
    unsolicited.clear();
    clear_unsolicited_front_cache();
    next_output_sequence = 1;
    close_requested = false;
    transaction.reset();
  }
};

struct Client : EndpointSession {
  Client(int client_fd, std::size_t unsolicited_output_bytes,
         std::size_t transaction_buffer_bytes,
         std::uint64_t assigned_connection_id, bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        fd(client_fd) {}

  int fd{-1};
  bool read_backpressured{false};
  bool close_after_write{false};
};

// Server-side state for one shared-memory ring: the mapping plus a RESP parser
// and reply buffer, mirroring a network Client but reading from the SQ and writing
// to the CQ instead of a socket.
struct RingEndpoint : EndpointSession {
  RingEndpoint(ring::Mapping&& ring_mapping,
               std::size_t unsolicited_output_bytes,
               std::size_t transaction_buffer_bytes,
               std::uint64_t assigned_connection_id,
               bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        mapping(std::move(ring_mapping)),
        sq(mapping.sq_consumer()),
        cq(mapping.cq_producer()) {}

  ring::Mapping mapping;
  // Long-lived views: Producer's cached_head_ only pays off if the object
  // survives across replies (recreating cq_producer() every record re-seeds it).
  // Rebind both after drain_for_reconnect so local cursors match the new indices.
  ring::Consumer sq;
  ring::Producer cq;
  // Local mirror of mapping.acked_epoch(); avoids a second shared-memory load on
  // every empty poll of the reconnect check.
  std::uint64_t acked_epoch{0};

  void rebind_ring_views() noexcept {
    sq = mapping.sq_consumer();
    cq = mapping.cq_producer();
  }
};

#ifdef GOBLIN_HAS_RDMA
struct RdmaEndpoint : EndpointSession {
  RdmaEndpoint(std::unique_ptr<rdma::Connection> rdma_connection,
               std::size_t unsolicited_output_bytes,
               std::size_t transaction_buffer_bytes,
               std::uint64_t assigned_connection_id,
               bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        connection(std::move(rdma_connection)) {}

  std::unique_ptr<rdma::Connection> connection;
  bool disconnect_started{false};
};

// The listener is declared before the endpoints so destruction runs in the
// reverse order: every QP/CM id is torn down before its shared event channel.
struct RdmaRuntimeTarget {
  std::unique_ptr<rdma::ServerListener> listener;
  std::vector<std::unique_ptr<RdmaEndpoint>> endpoints;
  std::size_t next_endpoint{0};
  bool listener_error_reported{false};
};
#endif

#ifdef GOBLIN_HAS_EXASOCK
// Priority TCP listener for `--exasock`. Clients are ordinary non-blocking
// sockets; under the exasock wrapper + ExaNIC bind they are accelerated.
struct ExasockRuntimeTarget {
  int listener_fd{-1};
  std::vector<std::unique_ptr<Client>> clients;
  std::size_t next_client{0};
};
#endif

struct PolledRuntimeTarget {
  std::unique_ptr<RingEndpoint> ring_endpoint;
#ifdef GOBLIN_HAS_RDMA
  std::unique_ptr<RdmaRuntimeTarget> rdma_target;
#endif
#ifdef GOBLIN_HAS_EXASOCK
  std::unique_ptr<ExasockRuntimeTarget> exasock_target;
#endif
};

[[nodiscard]] std::uint64_t next_connection_id() noexcept {
  static std::uint64_t next = 1;
  return next++;
}

void close_fd(int fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
  }
}

[[nodiscard]] bool would_block() noexcept {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

[[nodiscard]] bool has_pending_regular_output(
    const EndpointSession& session) noexcept {
  return session.reply_index < session.replies.size();
}

[[nodiscard]] bool has_pending_output(const EndpointSession& session) noexcept {
  return has_pending_regular_output(session) || !session.unsolicited.empty();
}

// Peek the unsolicited head, caching the mmap view across partial writes.
[[nodiscard]] bool peek_unsolicited(EndpointSession& session,
                                    detail::UnsolicitedOutputQueue::Front& out) noexcept {
  if (session.has_unsolicited_front) {
    out = session.unsolicited_front;
    return true;
  }
  auto front = session.unsolicited.front();
  if (!front) {
    return false;
  }
  session.unsolicited_front = *front;
  session.has_unsolicited_front = true;
  out = session.unsolicited_front;
  return true;
}

void consume_unsolicited_front(EndpointSession& session) noexcept {
  const std::size_t payload_len = session.unsolicited_front.bytes.size();
  session.unsolicited.pop_front(payload_len);
  session.clear_unsolicited_front_cache();
}

[[nodiscard]] std::size_t pending_output_bytes(
    const EndpointSession& session) noexcept {
  const std::size_t regular = session.output.size() - session.output_offset;
  const std::size_t unsolicited =
      session.unsolicited.payload_bytes() >= session.unsolicited_front_offset
          ? session.unsolicited.payload_bytes() - session.unsolicited_front_offset
          : 0;
  return regular + unsolicited;
}

void update_read_backpressure(Client& client, const ServerConfig& config) {
  if (config.max_output_buffer_bytes == 0) {
    client.read_backpressured = false;
    return;
  }

  const auto pending = pending_output_bytes(client);
  if (client.read_backpressured) {
    if (pending <= config.resume_output_buffer_bytes) {
      client.read_backpressured = false;
    }
    return;
  }

  if (pending >= config.max_output_buffer_bytes) {
    client.read_backpressured = true;
  }
}

void compact_output_if_needed(EndpointSession& session) {
  if (session.output_offset == 0) {
    return;
  }

  if (session.output_offset >= session.output.size()) {
    session.output.clear();
    session.output_offset = 0;
    session.replies.clear();
    session.reply_index = 0;
    return;
  }

  const auto remaining = session.output.size() - session.output_offset;
  if (session.output_offset >= kOutputCompactThreshold &&
      session.output_offset >= remaining) {
    const auto removed = session.output_offset;
    session.output.erase(0, removed);
    for (std::size_t i = session.reply_index; i < session.replies.size(); ++i) {
      session.replies[i].end_offset -= removed;
    }
    if (session.reply_index != 0) {
      session.replies.erase(session.replies.begin(),
                            session.replies.begin() +
                                static_cast<std::ptrdiff_t>(session.reply_index));
      session.reply_index = 0;
    }
    session.output_offset = 0;
  }
}

[[nodiscard]] bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

void set_tcp_nodelay(int fd) {
  int enabled = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
    std::cerr << "goblin-core: setsockopt(TCP_NODELAY) failed: " << std::strerror(errno)
              << '\n';
  }
}

struct SocketListenerRuntime {
  int fd{-1};
  bool tcp{false};
  std::string description;
  std::string uds_path;
};

[[nodiscard]] std::string format_tcp_endpoint(std::string_view address,
                                              std::uint16_t port) {
  std::string result;
  if (address.find(':') != std::string_view::npos) {
    result.push_back('[');
    result.append(address);
    result.push_back(']');
  } else {
    result.assign(address);
  }
  result.push_back(':');
  result.append(std::to_string(port));
  return result;
}

[[nodiscard]] std::optional<SocketListenerRuntime> create_unix_listener(
    const UdsListenerConfig& config, int backlog) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket(AF_UNIX) failed: " << std::strerror(errno)
              << '\n';
    return std::nullopt;
  }
  set_no_sigpipe(fd);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (config.path.size() >= sizeof(address.sun_path)) {
    std::cerr << "goblin-core: unix socket path too long: " << config.path
              << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  std::memcpy(address.sun_path, config.path.c_str(), config.path.size() + 1);
  ::unlink(config.path.c_str());  // clear any stale socket file

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "goblin-core: bind(unix " << config.path
              << ") failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::listen(fd, backlog) != 0) {
    std::cerr << "goblin-core: listen failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    ::unlink(config.path.c_str());
    return std::nullopt;
  }
  if (!set_nonblocking(fd)) {
    std::cerr << "goblin-core: failed to set listener nonblocking: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    ::unlink(config.path.c_str());
    return std::nullopt;
  }
  return SocketListenerRuntime{.fd = fd,
                               .tcp = false,
                               .description = "unix:" + config.path,
                               .uds_path = config.path};
}

[[nodiscard]] std::optional<SocketListenerRuntime> create_network_listener(
    const TcpListenerConfig& config, int backlog) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(config.port);
  const int lookup = ::getaddrinfo(config.bind_address.c_str(),
                                   port_text.c_str(), &hints, &resolved);
  if (lookup != 0) {
    std::cerr << "goblin-core: invalid TCP bind address "
              << format_tcp_endpoint(config.bind_address, config.port) << ": "
              << ::gai_strerror(lookup) << '\n';
    return std::nullopt;
  }

  int last_error = 0;
  for (const addrinfo* candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    const int fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                            candidate->ai_protocol);
    if (fd < 0) {
      last_error = errno;
      continue;
    }

    int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled)) != 0) {
      last_error = errno;
      close_fd(fd);
      continue;
    }
    if (candidate->ai_family == AF_INET6 &&
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
                     sizeof(enabled)) != 0) {
      last_error = errno;
      close_fd(fd);
      continue;
    }
    set_no_sigpipe(fd);

    if (::bind(fd, candidate->ai_addr,
               static_cast<socklen_t>(candidate->ai_addrlen)) != 0 ||
        ::listen(fd, backlog) != 0 || !set_nonblocking(fd)) {
      last_error = errno;
      close_fd(fd);
      continue;
    }

    ::freeaddrinfo(resolved);
    return SocketListenerRuntime{
        .fd = fd,
        .tcp = true,
        .description = format_tcp_endpoint(config.bind_address, config.port),
        .uds_path = {}};
  }

  ::freeaddrinfo(resolved);
  std::cerr << "goblin-core: bind/listen "
            << format_tcp_endpoint(config.bind_address, config.port)
            << " failed: " << std::strerror(last_error) << '\n';
  return std::nullopt;
}

void close_socket_listeners(
    std::vector<SocketListenerRuntime>& listeners) noexcept {
  for (auto& listener : listeners) {
    close_fd(listener.fd);
    listener.fd = -1;
    if (!listener.uds_path.empty()) {
      (void)::unlink(listener.uds_path.c_str());
    }
  }
}

[[nodiscard]] std::optional<std::vector<SocketListenerRuntime>>
create_socket_listeners(const ServerConfig& config) {
  std::vector<SocketListenerRuntime> listeners;
  listeners.reserve(config.socket_listeners.size());
  for (const auto& configured : config.socket_listeners) {
    std::optional<SocketListenerRuntime> listener;
    if (const auto* tcp = std::get_if<TcpListenerConfig>(&configured)) {
      listener = create_network_listener(*tcp, config.backlog);
    } else {
      listener = create_unix_listener(std::get<UdsListenerConfig>(configured),
                                      config.backlog);
    }
    if (!listener) {
      close_socket_listeners(listeners);
      return std::nullopt;
    }
    listeners.push_back(std::move(*listener));
  }
  return listeners;
}

void accept_clients(int listener_fd, bool tcp,
                    std::vector<std::unique_ptr<Client>>& clients,
                    const ServerConfig& config) {
  for (;;) {
    sockaddr_storage address{};
    socklen_t address_len = sizeof(address);
    const int client_fd =
        ::accept(listener_fd, reinterpret_cast<sockaddr*>(&address), &address_len);
    if (client_fd < 0) {
      if (would_block()) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "goblin-core: accept failed: " << std::strerror(errno) << '\n';
      return;
    }

    if (!set_nonblocking(client_fd)) {
      std::cerr << "goblin-core: failed to set client nonblocking: " << std::strerror(errno)
                << '\n';
      close_fd(client_fd);
      continue;
    }
    set_no_sigpipe(client_fd);
    if (tcp) {
      set_tcp_nodelay(client_fd);
    }

    try {
      auto client = std::make_unique<Client>(
          client_fd, config.unsolicited_output_buffer_bytes,
          config.transaction_buffer_bytes, next_connection_id(),
          config.auth_file.has_value());
      if (config.initial_output_buffer_bytes > 0) {
        client->output.reserve(config.initial_output_buffer_bytes);
      }
      clients.push_back(std::move(client));
    } catch (const std::bad_alloc&) {
      std::cerr << "goblin-core: unable to map client buffers\n";
      close_fd(client_fd);
    }
  }
}

[[nodiscard]] bool is_pubsub_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::subscribe:
    case CommandType::unsubscribe:
    case CommandType::psubscribe:
    case CommandType::punsubscribe:
    case CommandType::publish:
    case CommandType::pubsub:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool allowed_while_resp2_subscribed(CommandType type) noexcept {
  return type == CommandType::subscribe || type == CommandType::unsubscribe ||
         type == CommandType::psubscribe || type == CommandType::punsubscribe ||
         type == CommandType::ping || type == CommandType::quit;
}

[[nodiscard]] bool allowed_before_authentication(CommandType type) noexcept {
  return type == CommandType::auth || type == CommandType::hello ||
         type == CommandType::quit;
}

[[nodiscard]] bool equals_ascii_ci(std::string_view lhs,
                                   std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto upper = [](unsigned char byte) {
      return byte >= 'a' && byte <= 'z'
                 ? static_cast<unsigned char>(byte - ('a' - 'A'))
                 : byte;
    };
    if (upper(static_cast<unsigned char>(lhs[i])) !=
        upper(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

void scrub_auth_request(std::span<const std::string_view> fields) noexcept {
  if (fields.empty() ||
      (!equals_ascii_ci(fields.front(), "AUTH") &&
       !equals_ascii_ci(fields.front(), "HELLO"))) {
    return;
  }
  for (const auto field : fields.subspan(1)) {
    secure_zero_memory(const_cast<char*>(field.data()), field.size());
  }
}

void append_transaction_limit_error(std::string& out,
                                    std::size_t mapped_bytes) {
  resp::append_error(
      out, "ERR transaction exceeds the configured " +
               std::to_string(mapped_bytes) + "-byte buffer limit");
}

void execute_queued_transaction(EndpointSession& session, Store& store,
                                detail::PubSubRegistry& pubsub,
                                detail::WatchRegistry& watches,
                                const CommandExecutionOptions& exec_options) {
  auto& transaction = session.transaction;
  if (!transaction.in_multi) {
    resp::append_error(session.output, "ERR EXEC without MULTI");
    return;
  }

  const bool watch_dirty = transaction.watch_dirty;
  watches.remove(transaction);
  if (watch_dirty) {
    transaction.finish();
    if (session.resp_version == resp::Version::resp3) {
      resp::append_null(session.output, session.resp_version);
    } else {
      session.output.append("*-1\r\n");
    }
    return;
  }

  const auto failure = transaction.failure;
  if (failure != detail::TransactionFailure::none) {
    const auto capacity = transaction.commands.mapped_bytes();
    transaction.finish();
    if (failure == detail::TransactionFailure::buffer_limit) {
      resp::append_error(
          session.output,
          "EXECABORT Transaction discarded because its " +
              std::to_string(capacity) + "-byte buffer limit was exceeded.");
    } else {
      resp::append_error(
          session.output,
          "EXECABORT Transaction discarded because of previous errors.");
    }
    return;
  }

  const std::size_t count = transaction.commands.command_count();
  resp::append_array_header(session.output, count);
  transaction.in_multi = false;
  std::size_t offset = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (!transaction.commands.decode(offset, session.fields)) {
      resp::append_error(session.output, "ERR corrupt transaction buffer");
      break;
    }
    auto parsed = parse_command(session.fields);
    if (!parsed.ok()) {
      resp::append_error(session.output, parsed.error);
    } else if (parsed.command->type == CommandType::unwatch) {
      // UNWATCH is queueable. EXEC has already removed the transaction's watch
      // registrations, so its ordered result is simply OK.
      resp::append_simple_string(session.output, "OK");
    } else if (is_pubsub_command(parsed.command->type)) {
      pubsub.execute(session, *parsed.command, session.output);
    } else {
      execute_command_into(store, *parsed.command, session.output, exec_options);
    }
    scrub_auth_request(session.fields);
  }
  transaction.finish();
}

[[nodiscard]] bool execute_transaction_control(
    EndpointSession& session, Store& store, detail::PubSubRegistry& pubsub,
    detail::WatchRegistry& watches, const Command& command,
    std::span<const std::string_view> fields,
    const CommandExecutionOptions& exec_options) {
  auto& transaction = session.transaction;
  switch (command.type) {
    case CommandType::multi:
      if (transaction.in_multi) {
        resp::append_error(session.output, "ERR MULTI calls can not be nested");
      } else {
        transaction.begin();
        resp::append_simple_string(session.output, "OK");
      }
      return true;
    case CommandType::exec:
      execute_queued_transaction(session, store, pubsub, watches, exec_options);
      return true;
    case CommandType::discard:
      if (!transaction.in_multi) {
        resp::append_error(session.output, "ERR DISCARD without MULTI");
      } else {
        watches.remove(transaction);
        transaction.finish();
        resp::append_simple_string(session.output, "OK");
      }
      return true;
    case CommandType::watch:
      if (transaction.in_multi) {
        resp::append_error(session.output, "ERR WATCH inside MULTI is not allowed");
        return true;
      }
      for (const auto key : command.args) {
        (void)store.purge_if_expired(key, store.now_ms());
      }
      try {
        watches.watch(transaction, command.args);
        resp::append_simple_string(session.output, "OK");
      } catch (const std::bad_alloc&) {
        resp::append_error(session.output, "ERR unable to register watched keys");
      }
      return true;
    case CommandType::unwatch:
      if (!transaction.in_multi) {
        watches.remove(transaction);
        resp::append_simple_string(session.output, "OK");
        return true;
      }
      break;  // UNWATCH is queued inside MULTI.
    default:
      break;
  }

  if (!transaction.in_multi) {
    return false;
  }
  if (transaction.failure == detail::TransactionFailure::buffer_limit) {
    append_transaction_limit_error(session.output,
                                   transaction.commands.mapped_bytes());
    return true;
  }
  if (!transaction.commands.append(fields)) {
    transaction.failure = detail::TransactionFailure::buffer_limit;
    append_transaction_limit_error(session.output,
                                   transaction.commands.mapped_bytes());
  } else {
    resp::append_simple_string(session.output, "QUEUED");
  }
  return true;
}

void dispatch_resp_command(EndpointSession& session, Store& store,
                           detail::PubSubRegistry& pubsub,
                           detail::WatchRegistry& watches,
                           std::span<const std::string_view> fields,
                           const CommandExecutionOptions& exec_options) {
  const std::size_t prior_size = session.output.size();
  auto parsed = parse_command(fields);
  if (!parsed.ok()) {
    if (session.transaction.in_multi &&
        session.transaction.failure != detail::TransactionFailure::buffer_limit) {
      session.transaction.failure = detail::TransactionFailure::command_error;
    }
    resp::append_error(session.output, parsed.error);
    session.record_reply(prior_size);
    scrub_auth_request(fields);
    return;
  }

  const auto& command = *parsed.command;
  if (session.transaction.in_multi && command.type == CommandType::unknown) {
    if (session.transaction.failure != detail::TransactionFailure::buffer_limit) {
      session.transaction.failure = detail::TransactionFailure::command_error;
    }
    execute_command_into(store, command, session.output, exec_options);
    scrub_auth_request(fields);
    session.record_reply(prior_size);
    return;
  }
  if (session.authentication_required && !session.authenticated &&
      !allowed_before_authentication(command.type)) {
    resp::append_error(session.output, "NOAUTH Authentication required.");
  } else if (session.wire_mode == detail::WireMode::resp2 &&
      session.subscription_count() != 0 &&
      !allowed_while_resp2_subscribed(command.type)) {
    resp::append_error(
        session.output,
        "ERR Can't execute this command while subscribed to a channel");
  } else if (session.wire_mode == detail::WireMode::resp2 &&
             session.subscription_count() != 0 &&
             command.type == CommandType::ping) {
    resp::append_array_header(session.output, 2);
    resp::append_bulk_string(session.output, "pong");
    resp::append_bulk_string(session.output,
                             command.args.empty() ? std::string_view{}
                                                  : command.args.front());
  } else if (execute_transaction_control(session, store, pubsub, watches,
                                         command, fields, exec_options)) {
    if (command.type == CommandType::exec) {
      // EXEC reuses session.fields to decode mmap records, invalidating the
      // caller's span. EXEC has no credentials of its own, so skip the common
      // input scrub and finish the reply here.
      if (session.wire_mode != detail::WireMode::sbe) {
        session.wire_mode = session.resp_version == resp::Version::resp3
                                ? detail::WireMode::resp3
                                : detail::WireMode::resp2;
      }
      session.record_reply(prior_size);
      return;
    }
  } else if (is_pubsub_command(command.type)) {
    pubsub.execute(session, command, session.output);
  } else {
    execute_command_into(store, command, session.output, exec_options);
  }
  scrub_auth_request(fields);

  if (session.wire_mode != detail::WireMode::sbe) {
    session.wire_mode = session.resp_version == resp::Version::resp3
                            ? detail::WireMode::resp3
                            : detail::WireMode::resp2;
  }
  session.record_reply(prior_size);
}

#ifdef GOBLIN_HAS_SBE
[[nodiscard]] std::size_t dispatch_sbe_command(
    EndpointSession& session, Store& store, detail::PubSubRegistry& pubsub,
    std::string_view bytes, const CommandExecutionOptions& exec_options) {
  if (bytes.size() < kSbeLenPrefix) {
    return 0;
  }
  std::uint32_t message_length = 0;
  std::memcpy(&message_length, bytes.data(), kSbeLenPrefix);
  const std::size_t frame_bytes = kSbeLenPrefix + message_length;
  if (bytes.size() < frame_bytes) {
    return 0;
  }
  if (message_length < goblin_sbe::MessageHeader::encodedLength()) {
    return frame_bytes;
  }

  char* buffer = const_cast<char*>(bytes.data()) + kSbeLenPrefix;
  try {
    goblin_sbe::MessageHeader header(buffer, message_length);
    const auto template_id = header.templateId();
    Command command;

    if (template_id == goblin_sbe::Subscribe::sbeTemplateId()) {
      goblin_sbe::Subscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.channels();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getChannelAsStringView());
      }
      command.type = CommandType::subscribe;
      command.name = "SUBSCRIBE";
    } else if (template_id == goblin_sbe::Unsubscribe::sbeTemplateId()) {
      goblin_sbe::Unsubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.channels();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getChannelAsStringView());
      }
      command.type = CommandType::unsubscribe;
      command.name = "UNSUBSCRIBE";
    } else if (template_id == goblin_sbe::PSubscribe::sbeTemplateId()) {
      goblin_sbe::PSubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.patterns();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getPatternAsStringView());
      }
      command.type = CommandType::psubscribe;
      command.name = "PSUBSCRIBE";
    } else if (template_id == goblin_sbe::PUnsubscribe::sbeTemplateId()) {
      goblin_sbe::PUnsubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.patterns();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getPatternAsStringView());
      }
      command.type = CommandType::punsubscribe;
      command.name = "PUNSUBSCRIBE";
    } else if (template_id == goblin_sbe::Publish::sbeTemplateId()) {
      goblin_sbe::Publish message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      session.fields.clear();
      session.fields.push_back(message.getChannelAsStringView());
      session.fields.push_back(message.getPayloadAsStringView());
      command.type = CommandType::publish;
      command.name = "PUBLISH";
    } else if (template_id == goblin_sbe::PubSub::sbeTemplateId()) {
      goblin_sbe::PubSub message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      const auto operation = message.operation();
      session.fields.clear();
      if (operation == 0) {
        session.fields.push_back("CHANNELS");
      } else if (operation == 1) {
        session.fields.push_back("NUMSUB");
      } else if (operation == 2) {
        session.fields.push_back("NUMPAT");
      } else {
        session.fields.push_back("UNKNOWN");
      }
      auto& group = message.args();
      session.fields.reserve(1 + static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getArgAsStringView());
      }
      command.type = CommandType::pubsub;
      command.name = "PUBSUB";
    } else {
      return sbe_dispatch_one(store, bytes, session.output, exec_options);
    }

    command.args = session.fields;
    pubsub.execute(session, command, session.output);
  } catch (const std::exception&) {
    // Consume malformed frames and re-synchronize at the next length prefix.
  }
  return frame_bytes;
}
#endif

[[nodiscard]] bool process_buffered_commands(Client& client,
                                             Store& store,
                                             detail::PubSubRegistry& pubsub,
                                             detail::WatchRegistry& watches,
                                             ScriptEngine& script_engine,
                                             LuauEngine& luau_engine,
                                             WrenEngine& wren_engine,
                                             TclEngine& tcl_engine,
                                             UPythonEngine& upython_engine,
                                             QuickJsEngine& quickjs_engine,
                                             const ServerConfig& config,
                                             const AuthDatabase* auth_database) {
  compact_output_if_needed(client);
  update_read_backpressure(client, config);

  const CommandExecutionOptions exec_options{
      .output_reserve_limit = config.max_output_buffer_bytes,
      .resp_version = &client.resp_version,
      .connection_id = client.connection_id,
      .auth_database = auth_database,
      .authenticated = &client.authenticated,
      .authenticated_username = &client.authenticated_username,
      .client_name = &client.client_name,
      .client_library_name = &client.client_library_name,
      .client_library_version = &client.client_library_version,
      .quit_requested = &client.close_after_write,
      .script_engine = &script_engine,
      .luau_engine = &luau_engine,
      .wren_engine = &wren_engine,
      .tcl_engine = &tcl_engine,
      .upython_engine = &upython_engine,
      .quickjs_engine = &quickjs_engine};

  // Decide the protocol from the first 8 bytes ("GOBLINS!" -> SBE, else RESP),
  // mirroring the ring. Decide RESP the moment the prefix diverges so a short inline
  // command is never stalled waiting for a full magic.
  if (client.wire_mode == detail::WireMode::undecided) {
#ifdef GOBLIN_HAS_SBE
    if (config.enable_sbe) {
      switch (match_goblin_magic(client.inbuf)) {
        case MagicMatch::need_more:
          return true;  // wait for the next read before deciding
        case MagicMatch::yes:
          client.wire_mode = detail::WireMode::sbe;
          client.authentication_required = false;
          client.authenticated = true;
          client.authenticated_username.clear();
          client.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
          break;
        case MagicMatch::no:
          client.wire_mode = detail::WireMode::resp2;
          break;
      }
    } else {
      client.wire_mode = detail::WireMode::resp2;
    }
#else
    client.wire_mode = detail::WireMode::resp2;
#endif
  }

#ifdef GOBLIN_HAS_SBE
  if (client.wire_mode == detail::WireMode::sbe) {
    // Frame and dispatch every complete SBE message; a partial trailing message stays
    // buffered for the next read.
    std::size_t off = 0;
    while (!client.read_backpressured) {
      const std::size_t prior_size = client.output.size();
      const std::size_t consumed = dispatch_sbe_command(
          client, store, pubsub, std::string_view(client.inbuf).substr(off),
          exec_options);
      if (consumed == 0) {
        break;
      }
      client.record_reply(prior_size);
      off += consumed;
      compact_output_if_needed(client);
      update_read_backpressure(client, config);
    }
    if (off > 0) {
      client.inbuf.erase(0, off);
    }
    return true;
  }
#endif

  // RESP: move the accumulated bytes into the parser, then pop and dispatch.
  client.parser.append(client.inbuf);
  secure_zero_memory(client.inbuf.data(), client.inbuf.size());
  client.inbuf.clear();
  while (!client.read_backpressured) {
    if (!client.parser.pop_into(client.fields)) {
      break;
    }
    dispatch_resp_command(client, store, pubsub, watches, client.fields,
                          exec_options);
    if (client.close_after_write) {
      break;
    }
    compact_output_if_needed(client);
    update_read_backpressure(client, config);
  }

  if (client.parser.has_error()) {
    if (!client.close_after_write) {
      const std::size_t prior_size = client.output.size();
      resp::append_error(client.output, client.parser.error());
      client.record_reply(prior_size);
    }
    client.close_after_write = true;
    update_read_backpressure(client, config);
  }

  return true;
}

// Whether a client has a complete command buffered that can be dispatched without a
// new socket read -- RESP frames queued in the parser, or a full SBE frame in inbuf.
// Drives the post-backpressure drain loop; a partial SBE frame returns false so that
// loop cannot spin.
[[nodiscard]] bool has_buffered_work(const Client& client) {
#ifdef GOBLIN_HAS_SBE
  if (client.wire_mode == detail::WireMode::sbe) {
    if (client.inbuf.size() < kSbeLenPrefix) {
      return false;
    }
    std::uint32_t len = 0;
    std::memcpy(&len, client.inbuf.data(), kSbeLenPrefix);
    return client.inbuf.size() >= kSbeLenPrefix + len;
  }
#endif
  return client.parser.has_queued_frames();
}

[[nodiscard]] bool read_client(Client& client, Store& store,
                               detail::PubSubRegistry& pubsub,
                               detail::WatchRegistry& watches,
                               ScriptEngine& script_engine,
                               LuauEngine& luau_engine,
                               WrenEngine& wren_engine,
                               TclEngine& tcl_engine,
                               UPythonEngine& upython_engine,
                               QuickJsEngine& quickjs_engine,
                               const ServerConfig& config,
                               const AuthDatabase* auth_database) {
  const std::size_t bufsize = config.client_read_buffer_bytes != 0 ? config.client_read_buffer_bytes : 16 * 1024;
  static thread_local std::vector<char> buffer;
  if (buffer.size() < bufsize) buffer.resize(bufsize);

  for (;;) {
    if (client.read_backpressured) {
      return true;
    }

    const auto received = ::recv(client.fd, buffer.data(), bufsize, 0);
    if (received > 0) {
      client.inbuf.append(buffer.data(), static_cast<std::size_t>(received));

      if (!process_buffered_commands(client, store, pubsub, watches,
                                     script_engine, luau_engine, wren_engine,
                                     tcl_engine, upython_engine, quickjs_engine,
                                     config, auth_database)) {
        return false;
      }
      if (client.close_after_write) {
        return true;
      }
      continue;
    }

    if (received == 0) {
      return false;
    }

    if (would_block()) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }

    return false;
  }
}

[[nodiscard]] bool write_client(Client& client, const ServerConfig& config) {
  if (client.close_requested) {
    return false;
  }
  while (has_pending_output(client)) {
    detail::UnsolicitedOutputQueue::Front push{};
    const bool has_push = peek_unsolicited(client, push);
    const bool regular_pending = has_pending_regular_output(client);
    const bool write_push = has_push &&
                            (!regular_pending ||
                             push.sequence <
                                 client.replies[client.reply_index].sequence);

    std::string_view bytes;
    if (write_push) {
      bytes = push.bytes.substr(client.unsolicited_front_offset);
    } else {
      // A pipeline can leave hundreds of adjacent replies in output. Preserve
      // their sequence relative to Pub/Sub pushes, but send every regular reply
      // before the next push as one byte range instead of one syscall per reply.
      auto last_reply = client.reply_index;
      while (last_reply + 1 < client.replies.size() &&
             (!has_push ||
              client.replies[last_reply + 1].sequence < push.sequence)) {
        ++last_reply;
      }
      const auto end = client.replies[last_reply].end_offset;
      bytes = std::string_view(client.output).substr(client.output_offset,
                                                     end - client.output_offset);
    }

    const auto sent = ::send(client.fd, bytes.data(), bytes.size(), 0);
    if (sent > 0) {
      const auto written = static_cast<std::size_t>(sent);
      if (write_push) {
        client.unsolicited_front_offset += written;
        if (client.unsolicited_front_offset == push.bytes.size()) {
          consume_unsolicited_front(client);
        }
      } else {
        client.output_offset += written;
        while (client.reply_index < client.replies.size() &&
               client.output_offset >=
                   client.replies[client.reply_index].end_offset) {
          ++client.reply_index;
        }
      }
      update_read_backpressure(client, config);
      continue;
    }

    if (sent < 0 && would_block()) {
      compact_output_if_needed(client);
      update_read_backpressure(client, config);
      return true;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }

    return false;
  }

  compact_output_if_needed(client);
  update_read_backpressure(client, config);
  return !client.close_after_write;
}

#ifdef GOBLIN_HAS_EXASOCK
// Create a non-blocking TCP listener for a priority ExaSock poll target.
[[nodiscard]] std::optional<int> create_tcp_listener(std::string_view address,
                                                     std::uint16_t port,
                                                     int backlog) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket failed: " << std::strerror(errno) << '\n';
    return std::nullopt;
  }
  int reuse = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    std::cerr << "goblin-core: setsockopt(SO_REUSEADDR) failed: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  set_no_sigpipe(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  const std::string address_storage(address);
  if (::inet_pton(AF_INET, address_storage.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "goblin-core: invalid IPv4 bind address: " << address << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "goblin-core: bind " << address << ':' << port
              << " failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::listen(fd, backlog) != 0) {
    std::cerr << "goblin-core: listen failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (!set_nonblocking(fd)) {
    std::cerr << "goblin-core: failed to set listener nonblocking: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  return fd;
}
#endif

// Polled mode. Create shared-memory rings, ExaSock TCP listeners, and RDMA
// listeners in their literal command-line order, then process one fragment from
// the highest-priority non-empty target and restart the scan. Only when every
// target is empty do we run one non-blocking plain-socket pass and cpu_relax().
template <class NetFn>
[[nodiscard]] bool run_polled_targets(
    const ServerConfig& config, Store& store, std::atomic_bool& running,
    detail::PubSubRegistry& pubsub, detail::WatchRegistry& watches,
    ScriptEngine& script_engine,
    LuauEngine& luau_engine, WrenEngine& wren_engine, TclEngine& tcl_engine,
    UPythonEngine& upython_engine, QuickJsEngine& quickjs_engine,
    const AuthDatabase* auth_database,
    NetFn&& network_iteration) {
  // macOS: same QoS as the client so both busy-pollers stay on P-cores. No-op
  // on Linux (pinning is done by the bench via taskset).
  ring::set_busy_poll_thread_realtime();

  std::vector<PolledRuntimeTarget> targets;
  targets.reserve(config.poll_targets.size());
  // Strictly bind allocations to the selected slice while polled targets are
  // created. main resolved this from --numa, --cpu, or unanimous hardware locality.
  const int ring_node = config.numa_node;
  if (ring_node >= 0) {
    (void)numa::bind_process_to_node(ring_node);
  }
  for (const auto& configured_target : config.poll_targets) {
    if (const auto* rc = std::get_if<RingConfig>(&configured_target)) {
      std::optional<ring::Mapping> mapping;
#if defined(__linux__)
      if (config.ring_hugetlb) {
        // Huge-page backing: create_hugetlb rounds the size up to the huge page,
        // places the file on a hugetlbfs mount, and symlinks rc.path to it.
        mapping = ring::Mapping::create_hugetlb(rc->path.c_str(), rc->bytes);
        if (!mapping) {
          std::cerr << "goblin-core: failed to create hugetlb ring " << rc->path
                    << " (no hugetlbfs mount, or no huge pages reserved -- see "
                       "/proc/meminfo HugePages_Free)\n";
          return false;
        }
      }
#endif
      if (!mapping) {
        const std::uint64_t cap = ring::capacity_for(rc->bytes);
        mapping = ring::Mapping::create(rc->path.c_str(), cap, cap);
      }
      if (!mapping) {
        std::cerr << "goblin-core: failed to create ring " << rc->path << ": "
                  << std::strerror(errno) << '\n';
        return false;
      }
      if (ring_node >= 0 && !mapping->numa_all_local(ring_node)) {
        std::cerr << "goblin-core: ring " << rc->path
                  << " could not be placed on NUMA node " << ring_node
                  << "; refusing to run a remote ring -- reserve memory"
                     " (or huge pages) on that node\n";
        return false;
      }
      std::cout << "goblin-core: ring " << rc->path << " ready ("
                << mapping->sq_capacity() << " bytes/direction"
                << (mapping->is_hugetlb() ? ", hugetlb" : "") << ")\n";
      try {
        PolledRuntimeTarget target;
        target.ring_endpoint = std::make_unique<RingEndpoint>(
            std::move(*mapping), config.unsolicited_output_buffer_bytes,
            config.transaction_buffer_bytes, next_connection_id(),
            auth_database != nullptr && !config.no_auth_ring);
        targets.push_back(std::move(target));
      } catch (const std::bad_alloc&) {
        std::cerr << "goblin-core: unable to map ring client buffers\n";
        return false;
      }
      continue;
    }

    if (const auto* ec = std::get_if<ExasockConfig>(&configured_target)) {
#ifdef GOBLIN_HAS_EXASOCK
      auto listener_fd =
          create_tcp_listener(ec->bind_address, ec->port, config.backlog);
      if (!listener_fd) {
        return false;
      }
      std::cout << "goblin-core: exasock " << ec->bind_address << ':'
                << ec->port << " ready (priority TCP";
      if (exasock::loaded()) {
        std::cout << ", ExaSock " << exasock::version_text() << " loaded";
      } else {
        std::cout << "; run under `exasock` for SmartNIC bypass";
      }
      std::cout << ")\n";
      PolledRuntimeTarget target;
      target.exasock_target = std::make_unique<ExasockRuntimeTarget>();
      target.exasock_target->listener_fd = *listener_fd;
      targets.push_back(std::move(target));
#else
      (void)ec;
      std::cerr << "goblin-core: this build cannot create an ExaSock poll "
                   "target (-DGOBLIN_CORE_ENABLE_EXASOCK=ON required)\n";
      return false;
#endif
      continue;
    }

    const auto& rc = std::get<RdmaConfig>(configured_target);
#ifdef GOBLIN_HAS_RDMA
    std::string error;
    auto listener = rdma::ServerListener::create(
        rc.bind_address, rc.port, rc.bytes, config.backlog, config.numa_node,
        error);
    if (!listener) {
      std::cerr << "goblin-core: failed to create RDMA listener "
                << rc.bind_address << ':' << rc.port << ": " << error << '\n';
      return false;
    }
    std::cout << "goblin-core: RDMA " << rc.bind_address << ':' << rc.port
              << " ready (" << rc.bytes << " slot bytes/peer/direction)\n";
    PolledRuntimeTarget target;
    target.rdma_target = std::make_unique<RdmaRuntimeTarget>();
    target.rdma_target->listener = std::move(listener);
    targets.push_back(std::move(target));
#else
    (void)rc;
    std::cerr << "goblin-core: this build cannot create an RDMA poll target\n";
    return false;
#endif
  }
  if (ring_node >= 0) {
    // Rings placed; restore the runtime allocation policy. Prefer the pinned node for
    // the arenas only when --numa-arena asked for it; otherwise go back to default.
    if (config.numa_arena) {
      (void)numa::prefer_node(ring_node);
    } else {
      numa::reset_policy();
    }
  }

#ifdef GOBLIN_HAS_SBE
  std::unique_ptr<detail::PubSubListenerRuntime> pubsub_listener;
  if (config.pubsub_listener) {
    try {
      pubsub_listener = std::make_unique<detail::PubSubListenerRuntime>(
          *config.pubsub_listener, config.pubsub_listener_pattern);
      std::cout << "goblin-core: Pub/Sub listener connected to "
                << pubsub_listener->description() << " and subscribed to pattern '"
                << config.pubsub_listener_pattern << "' over SBE\n";
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: Pub/Sub listener setup failed: "
                << error.what() << '\n';
      return false;
    }
  }
#endif

  std::cout << "goblin-core: polled mode -- busy-polling "
            << targets.size()
            << " ordered server target(s)"
            << (config.pubsub_listener ? " plus one Pub/Sub listener" : "")
            << " ahead of sockets (100% CPU by design)\n"
            << std::flush;

  // Drain one SQ record from `ep`, run whatever commands it completes, and push
  // the replies onto the CQ. Returns false when the ring's SQ was empty.
  const auto flush_polled_output = [&]<class Producer>(
                                       EndpointSession& ep,
                                       Producer& producer) -> bool {
    bool progressed = false;
    while (has_pending_output(ep)) {
      detail::UnsolicitedOutputQueue::Front push{};
      const bool has_push = peek_unsolicited(ep, push);
      const bool regular_pending = has_pending_regular_output(ep);
      const bool write_push = has_push &&
                              (!regular_pending ||
                               push.sequence < ep.replies[ep.reply_index].sequence);

      std::string_view bytes;
      if (write_push) {
        bytes = push.bytes.substr(ep.unsolicited_front_offset);
      } else {
        const auto end = ep.replies[ep.reply_index].end_offset;
        bytes = std::string_view(ep.output).substr(ep.output_offset,
                                                   end - ep.output_offset);
      }
      if (bytes.size() > producer.max_record_payload()) {
        bytes = bytes.substr(0, producer.max_record_payload());
      }
      if (!producer.try_push(bytes)) {
        break;
      }
      progressed = true;
      if (write_push) {
        ep.unsolicited_front_offset += bytes.size();
        if (ep.unsolicited_front_offset == push.bytes.size()) {
          consume_unsolicited_front(ep);
        }
      } else {
        ep.output_offset += bytes.size();
        if (ep.output_offset == ep.replies[ep.reply_index].end_offset) {
          ++ep.reply_index;
        }
      }
      compact_output_if_needed(ep);
    }
    return progressed;
  };

  const auto process_polled_endpoint = [&]<class Consumer, class Producer>(
                                           EndpointSession& ep,
                                           Consumer& consumer,
                                           Producer& producer) -> bool {
    const bool output_progress = flush_polled_output(ep, producer);
    if (ep.quit_after_write && !has_pending_output(ep)) {
      ep.close_requested = true;
      return true;
    }
    if (ep.close_requested || has_pending_output(ep)) {
      return output_progress;
    }

    const CommandExecutionOptions exec_options{
        .output_reserve_limit = config.max_output_buffer_bytes,
        .resp_version = &ep.resp_version,
        .connection_id = ep.connection_id,
        .auth_database = auth_database,
        .authenticated = &ep.authenticated,
        .authenticated_username = &ep.authenticated_username,
        .client_name = &ep.client_name,
        .client_library_name = &ep.client_library_name,
        .client_library_version = &ep.client_library_version,
        .quit_requested = &ep.quit_after_write,
        .script_engine = &script_engine,
        .luau_engine = &luau_engine,
        .wren_engine = &wren_engine,
        .tcl_engine = &tcl_engine,
        .upython_engine = &upython_engine,
        .quickjs_engine = &quickjs_engine};

    const auto record = consumer.peek();
    if (!record) {
      return false;
    }

    // Decide the protocol from the first 8 bytes: "GOBLINS!" -> the SBE binary wire,
    // else RESP. Decide RESP the moment the prefix diverges so a short inline RESP
    // command is never stalled waiting for a full magic that will not arrive.
    if (ep.wire_mode == detail::WireMode::undecided) {
      ep.inbuf.append(*record);
      consumer.pop();
#ifdef GOBLIN_HAS_SBE
      if (config.enable_sbe) {
        switch (match_goblin_magic(ep.inbuf)) {
          case MagicMatch::need_more:
            return true;  // matches so far but incomplete -> wait for the next record
          case MagicMatch::yes:
            ep.wire_mode = detail::WireMode::sbe;
            ep.authentication_required = false;
            ep.authenticated = true;
            ep.authenticated_username.clear();
            ep.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
            break;
          case MagicMatch::no:
            ep.wire_mode = detail::WireMode::resp2;
            break;
        }
      } else {
        ep.wire_mode = detail::WireMode::resp2;
      }
#else
      ep.wire_mode = detail::WireMode::resp2;
#endif
      // Magic may have been alone in the record; fall through to drain whatever
      // remains in inbuf (or nothing).
    } else {
#ifdef GOBLIN_HAS_SBE
      // Hot path (protocol already decided, no carry-over): dispatch SBE frames
      // straight out of the ring record -- no inbuf memcpy. Partial trailing
      // bytes fall back into inbuf for the next record.
      if (ep.wire_mode == detail::WireMode::sbe && ep.inbuf.empty()) {
        std::size_t off = 0;
        for (;;) {
          const std::size_t prior_size = ep.output.size();
          const std::size_t consumed = dispatch_sbe_command(
              ep, store, pubsub, std::string_view(*record).substr(off),
              exec_options);
          if (consumed == 0) {
            break;
          }
          ep.record_reply(prior_size);
          off += consumed;
        }
        if (off == record->size()) {
          consumer.pop();
        } else if (off > 0) {
          ep.inbuf.assign(record->data() + off, record->size() - off);
          consumer.pop();
        } else {
          // Incomplete first frame: buffer the whole record.
          ep.inbuf.assign(record->data(), record->size());
          consumer.pop();
        }
      } else
#endif
      {
        ep.inbuf.append(*record);
        consumer.pop();
      }
    }

#ifdef GOBLIN_HAS_SBE
    if (ep.wire_mode == detail::WireMode::sbe) {
      // Frame and dispatch every complete SBE message still in the accumulator
      // (carry-over from a prior partial frame, or bytes left after magic strip).
      if (!ep.inbuf.empty()) {
        std::size_t off = 0;
        for (;;) {
          const std::size_t prior_size = ep.output.size();
          const std::size_t consumed = dispatch_sbe_command(
              ep, store, pubsub, std::string_view(ep.inbuf).substr(off),
              exec_options);
          if (consumed == 0) {
            break;
          }
          ep.record_reply(prior_size);
          off += consumed;
        }
        if (off > 0) {
          ep.inbuf.erase(0, off);
        }
      }
    } else
#endif
    if (ep.wire_mode == detail::WireMode::resp2 ||
        ep.wire_mode == detail::WireMode::resp3) {
      ep.parser.append(ep.inbuf);
      secure_zero_memory(ep.inbuf.data(), ep.inbuf.size());
      ep.inbuf.clear();
      while (ep.parser.pop_into(ep.fields)) {
        dispatch_resp_command(ep, store, pubsub, watches, ep.fields,
                              exec_options);
        if (ep.quit_after_write) {
          break;
        }
      }
      if (ep.parser.has_error()) {
        const std::size_t prior_size = ep.output.size();
        resp::append_error(ep.output, ep.parser.error());
        ep.record_reply(prior_size);
        ep.parser.clear();  // resync the byte stream after a protocol error
      }
    }

    (void)flush_polled_output(ep, producer);
    return true;
  };

  const auto process_ring = [&](RingEndpoint& ep) -> bool {
    // A fresh shared-memory client asks the server to discard anything its dead
    // predecessor abandoned before it starts using the SPSC ring.
    if (const std::uint64_t epoch = ep.mapping.requested_epoch();
        epoch != ep.acked_epoch) {
      pubsub.remove(ep);
      watches.remove(ep.transaction);
      ep.mapping.drain_for_reconnect();
      ep.reset_connection(next_connection_id());
      ep.mapping.ack_epoch(epoch);
      ep.acked_epoch = epoch;
      ep.rebind_ring_views();
    }
    return process_polled_endpoint(ep, ep.sq, ep.cq);
  };

#ifdef GOBLIN_HAS_RDMA
  const auto process_rdma_target = [&](RdmaRuntimeTarget& target) -> bool {
    bool progressed = false;
    auto event = target.listener->poll();
    progressed = event.progressed;
    if (!target.listener->error().empty() && !target.listener_error_reported) {
      std::cerr << "goblin-core: RDMA listener error: "
                << target.listener->error() << '\n';
      target.listener_error_reported = true;
      running = false;
      return true;
    }
    if (event.connection) {
      try {
        target.endpoints.push_back(std::make_unique<RdmaEndpoint>(
            std::move(event.connection), config.unsolicited_output_buffer_bytes,
            config.transaction_buffer_bytes, next_connection_id(),
            auth_database != nullptr && !config.no_auth_rdma));
      } catch (const std::bad_alloc&) {
        std::cerr << "goblin-core: unable to allocate RDMA endpoint state\n";
        running = false;
      }
      progressed = true;
    }

    for (std::size_t i = target.endpoints.size(); i > 0; --i) {
      const std::size_t index = i - 1;
      auto& endpoint = *target.endpoints[index];
      if (!endpoint.connection->failed() &&
          !endpoint.connection->disconnected()) {
        continue;
      }
      if (endpoint.connection->failed() && !endpoint.connection->error().empty()) {
        std::cerr << "goblin-core: RDMA peer closed after error: "
                  << endpoint.connection->error() << '\n';
      }
      pubsub.remove(endpoint);
      watches.remove(endpoint.transaction);
      target.endpoints.erase(target.endpoints.begin() +
                             static_cast<std::ptrdiff_t>(index));
      if (target.next_endpoint > index) {
        --target.next_endpoint;
      }
      progressed = true;
    }
    if (target.endpoints.empty()) {
      target.next_endpoint = 0;
      return progressed;
    }
    target.next_endpoint %= target.endpoints.size();

    const std::size_t count = target.endpoints.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (target.next_endpoint + offset) % count;
      auto& endpoint = *target.endpoints[index];
      if (!endpoint.connection->established()) {
        continue;
      }
      if (endpoint.close_requested && !has_pending_output(endpoint)) {
        if (!endpoint.disconnect_started) {
          endpoint.connection->disconnect();
          endpoint.disconnect_started = true;
          target.next_endpoint = (index + 1) % count;
          return true;
        }
        continue;
      }
      if (process_polled_endpoint(endpoint, *endpoint.connection,
                                  *endpoint.connection)) {
        target.next_endpoint = (index + 1) % count;
        return true;
      }
    }
    return progressed;
  };
#endif

#ifdef GOBLIN_HAS_EXASOCK
  // One unit of work on a priority ExaSock TCP target: drain accepts, then
  // service one client that has readable/writable progress (round-robin).
  const auto process_exasock_target = [&](ExasockRuntimeTarget& target) -> bool {
    bool progressed = false;
    const auto before_accept = target.clients.size();
    accept_clients(target.listener_fd, true, target.clients, config);
    if (target.clients.size() > before_accept) {
      progressed = true;
    }

    // Close finished clients.
    for (std::size_t i = target.clients.size(); i > 0; --i) {
      const std::size_t index = i - 1;
      auto& client = *target.clients[index];
      if (client.close_requested && !has_pending_output(client)) {
        pubsub.remove(client);
        watches.remove(client.transaction);
        close_fd(client.fd);
        target.clients.erase(target.clients.begin() +
                             static_cast<std::ptrdiff_t>(index));
        if (target.next_client > index) {
          --target.next_client;
        }
        progressed = true;
      }
    }
    if (target.clients.empty()) {
      target.next_client = 0;
      return progressed;
    }
    target.next_client %= target.clients.size();

    const std::size_t count = target.clients.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (target.next_client + offset) % count;
      auto& client = *target.clients[index];
      update_read_backpressure(client, config);

      bool keep = !client.close_requested;
      bool did_work = false;

      if (keep && has_pending_output(client)) {
        const bool before_pending = has_pending_output(client);
        keep = write_client(client, config);
        if (before_pending) {
          did_work = true;
        }
      }

      if (keep && !client.read_backpressured && has_buffered_work(client)) {
        keep = process_buffered_commands(
            client, store, pubsub, watches, script_engine, luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config,
            auth_database);
        did_work = true;
      }

      // Non-blocking readiness probe (timeout 0) so we only enter read_client
      // when the socket has data -- matches the ring/RDMA "one fragment" unit.
      pollfd pfd{.fd = client.fd, .events = 0, .revents = 0};
      if (keep && !client.read_backpressured) {
        pfd.events |= POLLIN;
      }
      if (keep && has_pending_output(client)) {
        pfd.events |= POLLOUT;
      }
      if (pfd.events != 0) {
        (void)::poll(&pfd, 1, 0);
      }

      if (keep && (pfd.revents & POLLIN) != 0 && !client.read_backpressured) {
        keep = read_client(client, store, pubsub, watches, script_engine,
                           luau_engine, wren_engine, tcl_engine, upython_engine,
                           quickjs_engine, config, auth_database);
        did_work = true;
      }
      if (keep && (pfd.revents & POLLOUT) != 0 && has_pending_output(client)) {
        keep = write_client(client, config);
        did_work = true;
      }

      if (!keep || client.close_requested) {
        pubsub.remove(client);
        watches.remove(client.transaction);
        close_fd(client.fd);
        target.clients.erase(target.clients.begin() +
                             static_cast<std::ptrdiff_t>(index));
        if (target.next_client > index) {
          --target.next_client;
        } else if (!target.clients.empty()) {
          target.next_client %= target.clients.size();
        } else {
          target.next_client = 0;
        }
        return true;
      }

      if (did_work) {
        target.next_client =
            target.clients.empty() ? 0 : (index + 1) % target.clients.size();
        return true;
      }
    }
    return progressed;
  };
#endif

  // macOS has no core pinning; raise this busy-poll thread's priority so the scheduler
  // stops parking it mid-spin (a no-op elsewhere -- Linux uses taskset/isolcpus).
  ring::set_busy_poll_thread_realtime();

  // Idle spins between polled requests: running network_iteration (poll + active
  // expire) every empty loop is a p99 footgun. Service sockets every 64th idle
  // pass; a busy higher-priority target still starves everything below by design.
  //
  // Pure spin (no park): a multi-ring server cannot block on one SQ without
  // missing the others. On Apple Silicon cpu_relax() is a compiler barrier (not
  // YIELD), so the spin does not volunteer deschedule; the client's adaptive
  // wait parks the other side of the handoff when the peer is stalled.
  unsigned idle_spins = 0;
  while (running.load(std::memory_order_relaxed)) {
    bool progressed = false;
    for (auto& target : targets) {
      bool target_progress = false;
      if (target.ring_endpoint) {
        target_progress = process_ring(*target.ring_endpoint);
      }
#ifdef GOBLIN_HAS_EXASOCK
      else if (target.exasock_target) {
        target_progress = process_exasock_target(*target.exasock_target);
      }
#endif
#ifdef GOBLIN_HAS_RDMA
      else if (target.rdma_target) {
        target_progress = process_rdma_target(*target.rdma_target);
      }
#endif
      if (target_progress) {
        progressed = true;
        break;  // restart from the highest-priority configured target
      }
    }

#ifdef GOBLIN_HAS_SBE
    bool listener_progress = false;
    if (pubsub_listener) {
      try {
        listener_progress = pubsub_listener->rebroadcast_one(pubsub);
      } catch (const std::exception& error) {
        std::cerr << "goblin-core: Pub/Sub listener failed: " << error.what()
                  << '\n';
        running = false;
        break;
      }
      if (listener_progress) {
        // Relaying can enqueue output for ordinary TCP/UDS subscribers. Service
        // them now even when the upstream stream never goes idle.
        network_iteration(0);
        progressed = true;
      }
    }
#endif

    if (progressed) {
      idle_spins = 0;
      continue;
    }
    if ((++idle_spins & 63u) == 0) {
      network_iteration(0);  // sparse network pass while rings are quiet
    }
    ring::cpu_relax();
  }
  for (auto& target : targets) {
    if (target.ring_endpoint) {
      pubsub.remove(*target.ring_endpoint);
      watches.remove(target.ring_endpoint->transaction);
    }
#ifdef GOBLIN_HAS_EXASOCK
    if (target.exasock_target) {
      for (auto& client : target.exasock_target->clients) {
        pubsub.remove(*client);
        watches.remove(client->transaction);
        close_fd(client->fd);
        client->fd = -1;
      }
      close_fd(target.exasock_target->listener_fd);
      target.exasock_target->listener_fd = -1;
    }
#endif
#ifdef GOBLIN_HAS_RDMA
    if (target.rdma_target) {
      for (auto& endpoint : target.rdma_target->endpoints) {
        pubsub.remove(*endpoint);
        watches.remove(endpoint->transaction);
      }
    }
#endif
  }
  return true;
}

}  // namespace

Server::Server(ServerConfig config, Store& store)
    : config_(std::move(config)), store_(store) {
  if (config_.socket_listeners.empty()) {
    if (!config_.unix_socket_path.empty()) {
      config_.socket_listeners.emplace_back(
          UdsListenerConfig{.path = config_.unix_socket_path});
    } else {
      config_.socket_listeners.emplace_back(TcpListenerConfig{
          .bind_address = config_.bind_address, .port = config_.port});
    }
  }
  const std::size_t page = static_cast<std::size_t>(ring::page_size());
  const auto page_round = [page](std::size_t requested) {
    if (requested == 0) {
      return page;
    }
    if (requested <= std::numeric_limits<std::size_t>::max() - (page - 1)) {
      return ((requested + page - 1) / page) * page;
    }
    return (std::numeric_limits<std::size_t>::max() / page) * page;
  };
  config_.unsolicited_output_buffer_bytes =
      page_round(config_.unsolicited_output_buffer_bytes);
  config_.transaction_buffer_bytes =
      page_round(config_.transaction_buffer_bytes);
  if (config_.max_output_buffer_bytes == 0) {
    config_.resume_output_buffer_bytes = 0;
  } else if (config_.resume_output_buffer_bytes >= config_.max_output_buffer_bytes) {
    config_.resume_output_buffer_bytes = config_.max_output_buffer_bytes / 4;
  }
  if (config_.max_output_buffer_bytes > 0 &&
      config_.initial_output_buffer_bytes > config_.max_output_buffer_bytes) {
    config_.initial_output_buffer_bytes = config_.max_output_buffer_bytes;
  }
}

void Server::stop() noexcept {
  running_ = false;
}

int Server::run() {
  std::signal(SIGPIPE, SIG_IGN);
  bool kafka_failed = false;

#ifndef GOBLIN_HAS_SBE
  if (config_.enable_sbe) {
    std::cerr << "goblin-core: --enable-sbe requires an SBE-enabled build\n";
    return 1;
  }
#endif
  if (config_.pubsub_listener && !config_.enable_sbe) {
    std::cerr << "goblin-core: Pub/Sub listener transports require --enable-sbe\n";
    return 1;
  }

  std::optional<AuthDatabase> auth_database;
  if (config_.auth_file) {
    try {
      auth_database.emplace(AuthDatabase::load(*config_.auth_file));
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: cannot load auth file: " << error.what() << '\n';
      return 1;
    }
    std::cout << "goblin-core: RESP authentication enabled ("
              << auth_database->size() << " user(s))\n";
  }
  if (config_.enable_sbe) {
    std::cout << "goblin-core: SBE enabled as a trusted, unauthenticated fabric\n";
  }
  const AuthDatabase* auth = auth_database ? &*auth_database : nullptr;

#ifdef GOBLIN_HAS_KAFKA
  std::unique_ptr<KafkaIngestor> kafka;
  if (config_.kafka) {
    std::string error;
    kafka = KafkaIngestor::connect(config_.kafka->connection,
                                   config_.kafka->start_timestamp_ms, error);
    if (!kafka) {
      std::cerr << "goblin-core: Kafka setup failed: " << error << '\n';
      return 1;
    }
    KafkaReplayStats replay;
    std::cout << "goblin-core: replaying Kafka " << kafka->description()
              << " before opening listeners\n" << std::flush;
    if (!kafka->catch_up(store_, replay, error)) {
      std::cerr << "goblin-core: Kafka startup replay failed: " << error << '\n';
      return 1;
    }
    std::cout << "goblin-core: Kafka startup replay complete: "
              << replay.records << " record(s), " << replay.writes
              << " write(s), " << replay.filtered << " filtered\n";
  }
#else
  if (config_.kafka) {
    std::cerr << "goblin-core: this build has no Kafka support\n";
    return 1;
  }
#endif

  auto listeners_result = create_socket_listeners(config_);
  if (!listeners_result) {
    return 1;
  }
  auto listeners = std::move(*listeners_result);

  detail::PubSubRegistry pubsub;
  detail::WatchRegistry watches;
  store_.set_mutation_observer(StoreMutationObserver{
      .context = &watches,
      .key_modified = [](void* context, std::string_view key) noexcept {
        static_cast<detail::WatchRegistry*>(context)->modified(key);
      },
      .all_modified = [](void* context) noexcept {
        static_cast<detail::WatchRegistry*>(context)->modified_all();
      }});
  struct MutationObserverReset {
    Store* store;
    ~MutationObserverReset() { store->set_mutation_observer({}); }
  } mutation_observer_reset{&store_};
  std::vector<std::unique_ptr<Client>> clients;
  running_ = true;

  const NestedCommandDispatch nested_dispatch{
      .context = &pubsub,
      .publish = [](void* context, std::string_view channel,
                    std::string_view payload) {
        return static_cast<detail::PubSubRegistry*>(context)->publish(channel,
                                                                      payload);
      }};

  // One engine per interpreter for the process. Each holds its own script cache
  // and, lazily, its own VM -- no VM is created until the first script of that
  // kind runs, so a server that never scripts pays nothing here.
  ScriptEngine script_engine(store_, nested_dispatch);
  LuauEngine luau_engine(store_, nested_dispatch);
  WrenEngine wren_engine(store_, nested_dispatch);
  TclEngine tcl_engine(store_, nested_dispatch);
  UPythonEngine upython_engine(store_, nested_dispatch);
  QuickJsEngine quickjs_engine(store_, nested_dispatch);

  for (const auto& listener : listeners) {
    std::cout << "goblin-core listening on " << listener.description << '\n';
  }

  // Keys expired per active-expiration sweep. A bounded batch keeps a mass
  // expiry from stalling the loop; when a sweep fills the batch more are likely
  // due, so the poll below returns immediately to sweep again.
  constexpr std::size_t kActiveExpireBudget = 1000;
#ifdef GOBLIN_HAS_KAFKA
  constexpr std::size_t kKafkaDrainBudget = 1024;
  bool kafka_may_have_more = kafka && kafka->has_pending();
  const auto drain_kafka = [&]() -> bool {
    auto result = kafka->poll(store_, kKafkaDrainBudget);
    if (!result.ok()) {
      std::cerr << "goblin-core: Kafka ingestion failed: " << result.error
                << '\n';
      kafka_failed = true;
      running_ = false;
      return false;
    }
    kafka_may_have_more = result.may_have_more;
    return true;
  };
#endif

  // One pass over the network: reap a finished background save, do a bounded
  // active-expiration sweep, then poll() (blocking up to max_timeout_ms) and
  // service ready clients and new connections. In polled mode this is invoked only
  // when every ring/RDMA target is empty, always with timeout 0 (never blocking).
  const auto network_iteration = [&](int max_timeout_ms) {
#ifdef GOBLIN_HAS_KAFKA
    if (kafka && kafka_may_have_more && !drain_kafka()) {
      return;
    }
#endif
    if (auto outcome = store_.reap_background_save()) {
      if (outcome->ok) {
        std::cout << "goblin-core: background save of " << outcome->path
                  << " completed\n"
                  << std::flush;
      } else {
        std::cerr << "goblin-core: background save of " << outcome->path
                  << " FAILED\n";
      }
    }

    int poll_timeout = max_timeout_ms;
#ifdef GOBLIN_HAS_KAFKA
    if (kafka_may_have_more) poll_timeout = 0;
#endif
    if (!store_.ttl_empty()) {
      if (store_.active_expire(store_.now_ms(), kActiveExpireBudget) ==
          kActiveExpireBudget) {
        poll_timeout = 0;  // batch full -- more are likely due, sweep again now
      }
    }

    std::vector<pollfd> pollfds;
    const std::size_t listener_count = listeners.size();
    const std::size_t kafka_fd_count =
#ifdef GOBLIN_HAS_KAFKA
        kafka ? 1U : 0U;
#else
        0U;
#endif
    pollfds.reserve(clients.size() + listener_count + kafka_fd_count);
    for (const auto& listener : listeners) {
      pollfds.push_back(
          pollfd{.fd = listener.fd, .events = POLLIN, .revents = 0});
    }
#ifdef GOBLIN_HAS_KAFKA
    if (kafka) {
      pollfds.push_back(pollfd{.fd = kafka->notification_fd(),
                              .events = POLLIN,
                              .revents = 0});
    }
#endif
    for (auto& client_ptr : clients) {
      auto& client = *client_ptr;
      update_read_backpressure(client, config_);
      short events = 0;
      if (!client.read_backpressured) {
        events |= POLLIN;
      }
      if (has_pending_output(client)) {
        events |= POLLOUT;
      }
      pollfds.push_back(pollfd{.fd = client.fd, .events = events, .revents = 0});
    }

    const int ready = ::poll(pollfds.data(), pollfds.size(), poll_timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        return;
      }
      std::cerr << "goblin-core: poll failed: " << std::strerror(errno) << '\n';
      running_ = false;
      return;
    }

    std::vector<unsigned char> close_client(clients.size(), 0);
#ifdef GOBLIN_HAS_KAFKA
    if (kafka &&
        (pollfds[listener_count].revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
        !drain_kafka()) {
      return;
    }
#endif
    for (std::size_t i = 0; i < clients.size(); ++i) {
      bool keep = !clients[i]->close_requested;
      auto& client = *clients[i];
      const auto revents =
          pollfds[listener_count + kafka_fd_count + i].revents;

      if (keep && (revents & POLLOUT) != 0 && has_pending_output(client)) {
        keep = write_client(client, config_);
      }

      if (keep && !client.read_backpressured) {
        keep = process_buffered_commands(
            client, store_, pubsub, watches, script_engine, luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config_,
            auth);
      }

      if (keep && (revents & POLLIN) != 0 && !client.read_backpressured) {
        keep = read_client(client, store_, pubsub, watches, script_engine,
                           luau_engine, wren_engine, tcl_engine, upython_engine,
                           quickjs_engine, config_, auth);
      }

      if (keep && has_pending_output(client)) {
        keep = write_client(client, config_);
      }

      while (keep && !client.read_backpressured &&
             has_buffered_work(client)) {
        keep = process_buffered_commands(
            client, store_, pubsub, watches, script_engine, luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config_,
            auth);
        if (keep && has_pending_output(client)) {
          keep = write_client(client, config_);
        }
      }

      if (keep && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          !has_pending_output(client)) {
        keep = false;
      }

      if (!keep) {
        close_client[i] = 1;
      }
    }

    for (std::size_t i = clients.size(); i > 0; --i) {
      const auto index = i - 1;
      if (close_client[index] != 0) {
        pubsub.remove(*clients[index]);
        watches.remove(clients[index]->transaction);
        close_fd(clients[index]->fd);
        clients.erase(clients.begin() + static_cast<long>(index));
      }
    }

    for (std::size_t i = 0; i < listener_count; ++i) {
      if ((pollfds[i].revents & POLLIN) != 0) {
        accept_clients(listeners[i].fd, listeners[i].tcp, clients, config_);
      }
    }
  };

  if (config_.poll_targets.empty() && !config_.pubsub_listener) {
    // No polled targets: the ordinary event-driven server. poll() blocks, so an idle
    // server costs no CPU.
    while (running_) {
      network_iteration(1000);
    }
  } else if (!run_polled_targets(config_, store_, running_, pubsub, watches,
                                 script_engine, luau_engine, wren_engine,
                                 tcl_engine, upython_engine, quickjs_engine,
                                 auth, network_iteration)) {
    close_socket_listeners(listeners);
    return 1;
  }

  for (const auto& client : clients) {
    pubsub.remove(*client);
    watches.remove(client->transaction);
    close_fd(client->fd);
  }
  close_socket_listeners(listeners);

  return kafka_failed ? 1 : 0;
}

}  // namespace goblin::core
