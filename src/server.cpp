#include "goblin/core/server.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/luau_script.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/ring_buffer.hpp"
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

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
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
                  std::uint64_t assigned_connection_id)
      : PubSubSession(unsolicited_output_bytes),
        connection_id(assigned_connection_id) {}

  resp::Version resp_version{resp::Version::resp2};
  std::uint64_t connection_id{0};
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
  }
};

struct Client : EndpointSession {
  Client(int client_fd, std::size_t unsolicited_output_bytes,
         std::uint64_t assigned_connection_id)
      : EndpointSession(unsolicited_output_bytes, assigned_connection_id),
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
               std::uint64_t assigned_connection_id)
      : EndpointSession(unsolicited_output_bytes, assigned_connection_id),
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

[[nodiscard]] std::optional<int> create_unix_listener(const ServerConfig& config) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket(AF_UNIX) failed: " << std::strerror(errno)
              << '\n';
    return std::nullopt;
  }
  set_no_sigpipe(fd);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (config.unix_socket_path.size() >= sizeof(address.sun_path)) {
    std::cerr << "goblin-core: unix socket path too long: "
              << config.unix_socket_path << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  std::memcpy(address.sun_path, config.unix_socket_path.c_str(),
              config.unix_socket_path.size() + 1);
  ::unlink(config.unix_socket_path.c_str());  // clear any stale socket file

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "goblin-core: bind(unix) failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::listen(fd, config.backlog) != 0) {
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

[[nodiscard]] std::optional<int> create_listener(const ServerConfig& config) {
  if (!config.unix_socket_path.empty()) {
    return create_unix_listener(config);
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket failed: " << std::strerror(errno) << '\n';
    return std::nullopt;
  }

  int reuse = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    std::cerr << "goblin-core: setsockopt(SO_REUSEADDR) failed: " << std::strerror(errno)
              << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  set_no_sigpipe(fd);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(config.port);
  if (::inet_pton(AF_INET, config.bind_address.c_str(), &address.sin_addr) != 1) {
    std::cerr << "goblin-core: invalid IPv4 bind address: " << config.bind_address << '\n';
    close_fd(fd);
    return std::nullopt;
  }

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "goblin-core: bind failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }

  if (::listen(fd, config.backlog) != 0) {
    std::cerr << "goblin-core: listen failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }

  if (!set_nonblocking(fd)) {
    std::cerr << "goblin-core: failed to set listener nonblocking: " << std::strerror(errno)
              << '\n';
    close_fd(fd);
    return std::nullopt;
  }

  return fd;
}

void accept_clients(int listener,
                    std::vector<std::unique_ptr<Client>>& clients,
                    const ServerConfig& config) {
  for (;;) {
    sockaddr_storage address{};
    socklen_t address_len = sizeof(address);
    const int client_fd =
        ::accept(listener, reinterpret_cast<sockaddr*>(&address), &address_len);
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
    if (config.unix_socket_path.empty()) {
      set_tcp_nodelay(client_fd);  // no-op / warning on AF_UNIX
    }

    try {
      auto client = std::make_unique<Client>(
          client_fd, config.unsolicited_output_buffer_bytes, next_connection_id());
      if (config.initial_output_buffer_bytes > 0) {
        client->output.reserve(config.initial_output_buffer_bytes);
      }
      clients.push_back(std::move(client));
    } catch (const std::bad_alloc&) {
      std::cerr << "goblin-core: unable to map client output buffer\n";
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
         type == CommandType::ping;
}

void dispatch_resp_command(EndpointSession& session, Store& store,
                           detail::PubSubRegistry& pubsub,
                           std::span<const std::string_view> fields,
                           const CommandExecutionOptions& exec_options) {
  const std::size_t prior_size = session.output.size();
  auto parsed = parse_command(fields);
  if (!parsed.ok()) {
    resp::append_error(session.output, parsed.error);
    session.record_reply(prior_size);
    return;
  }

  const auto& command = *parsed.command;
  if (session.wire_mode == detail::WireMode::resp2 &&
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
  } else if (is_pubsub_command(command.type)) {
    pubsub.execute(session, command, session.output);
  } else {
    execute_command_into(store, command, session.output, exec_options);
  }

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
                                             ScriptEngine& script_engine,
                                             LuauEngine& luau_engine,
                                             WrenEngine& wren_engine,
                                             TclEngine& tcl_engine,
                                             UPythonEngine& upython_engine,
                                             QuickJsEngine& quickjs_engine,
                                             const ServerConfig& config) {
  compact_output_if_needed(client);
  update_read_backpressure(client, config);

  const CommandExecutionOptions exec_options{
      .output_reserve_limit = config.max_output_buffer_bytes,
      .resp_version = &client.resp_version,
      .connection_id = client.connection_id,
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
    switch (match_goblin_magic(client.inbuf)) {
      case MagicMatch::need_more:
        return true;  // wait for the next read before deciding
      case MagicMatch::yes:
        client.wire_mode = detail::WireMode::sbe;
        client.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
        break;
      case MagicMatch::no:
        client.wire_mode = detail::WireMode::resp2;
        break;
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
  client.inbuf.clear();
  while (!client.read_backpressured) {
    if (!client.parser.pop_into(client.fields)) {
      break;
    }
    dispatch_resp_command(client, store, pubsub, client.fields, exec_options);
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
                               ScriptEngine& script_engine,
                               LuauEngine& luau_engine,
                               WrenEngine& wren_engine,
                               TclEngine& tcl_engine,
                               UPythonEngine& upython_engine,
                               QuickJsEngine& quickjs_engine,
                               const ServerConfig& config) {
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

      if (!process_buffered_commands(client, store, pubsub, script_engine, luau_engine,
                                     wren_engine, tcl_engine, upython_engine,
                                     quickjs_engine, config)) {
        return false;
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
      const auto end = client.replies[client.reply_index].end_offset;
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
        if (client.output_offset ==
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

// Ring mode. Create every configured ring (the server is the reader/creator),
// then busy-poll them in priority order: process one SQ record from the
// highest-priority non-empty ring and restart the scan, so a busy ring starves
// the ones below it. Only when every ring is empty do we run one non-blocking
// network pass and cpu_relax(). Returns false if a ring could not be created.
template <class NetFn>
[[nodiscard]] bool run_rings(const ServerConfig& config, Store& store,
                             std::atomic_bool& running,
                             detail::PubSubRegistry& pubsub,
                             ScriptEngine& script_engine,
                             LuauEngine& luau_engine, WrenEngine& wren_engine,
                             TclEngine& tcl_engine, UPythonEngine& upython_engine,
                             QuickJsEngine& quickjs_engine,
                             NetFn&& network_iteration) {
  // macOS: same QoS as the client so both busy-pollers stay on P-cores. No-op
  // on Linux (pinning is done by the bench via taskset).
  ring::set_busy_poll_thread_realtime();

  std::vector<std::unique_ptr<RingEndpoint>> rings;
  rings.reserve(config.rings.size());
  // NUMA: with --cpu, strictly bind allocations to the pinned CPU's node while the
  // rings are created, so each ring's prefault lands there and it is node-local by
  // construction (verified per ring below). node_of_cpu() is -1 off Linux, so this is
  // a no-op there.
  const int ring_node = config.cpu >= 0 ? numa::node_of_cpu(config.cpu) : -1;
  if (ring_node >= 0) {
    (void)numa::bind_process_to_node(ring_node);
  }
  for (const auto& rc : config.rings) {
    std::optional<ring::Mapping> mapping;
#if defined(__linux__)
    if (config.ring_hugetlb) {
      // Huge-page backing: create_hugetlb rounds the size up to the huge page,
      // places the file on a hugetlbfs mount, and symlinks rc.path to it.
      mapping = ring::Mapping::create_hugetlb(rc.path.c_str(), rc.bytes);
      if (!mapping) {
        std::cerr << "goblin-core: failed to create hugetlb ring " << rc.path
                  << " (no hugetlbfs mount, or no huge pages reserved -- see "
                     "/proc/meminfo HugePages_Free)\n";
        return false;
      }
    }
#endif
    if (!mapping) {
      const std::uint64_t cap = ring::capacity_for(rc.bytes);
      mapping = ring::Mapping::create(rc.path.c_str(), cap, cap);
    }
    if (!mapping) {
      std::cerr << "goblin-core: failed to create ring " << rc.path << ": "
                << std::strerror(errno) << '\n';
      return false;
    }
    if (ring_node >= 0 && !mapping->numa_all_local(ring_node)) {
      std::cerr << "goblin-core: ring " << rc.path
                << " could not be placed on NUMA node " << ring_node << " (CPU "
                << config.cpu
                << "); refusing to run a remote ring -- reserve memory"
                   " (or huge pages) on that node\n";
      return false;
    }
    std::cout << "goblin-core: ring " << rc.path << " ready ("
              << mapping->sq_capacity() << " bytes/direction"
              << (mapping->is_hugetlb() ? ", hugetlb" : "") << ")\n";
    try {
      rings.push_back(std::make_unique<RingEndpoint>(
          std::move(*mapping), config.unsolicited_output_buffer_bytes,
          next_connection_id()));
    } catch (const std::bad_alloc&) {
      std::cerr << "goblin-core: unable to map ring client output buffer\n";
      return false;
    }
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
  std::cout << "goblin-core: ring mode -- busy-polling " << rings.size()
            << " ring(s) ahead of the network (100% CPU by design)\n"
            << std::flush;

  // Drain one SQ record from `ep`, run whatever commands it completes, and push
  // the replies onto the CQ. Returns false when the ring's SQ was empty.
  const auto flush_ring_output = [&](RingEndpoint& ep) -> bool {
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
      if (bytes.size() > ep.cq.max_record_payload()) {
        bytes = bytes.substr(0, ep.cq.max_record_payload());
      }
      if (!ep.cq.try_push(bytes)) {
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

  const auto process_ring = [&](RingEndpoint& ep) -> bool {
    // Reconnect handshake: a newly-opened client bumps the ring epoch and spins until
    // we ack. When it moves, discard whatever a dead predecessor abandoned in the ring
    // (an unconsumed request, an unread reply), re-arm protocol detection, and ack so
    // the client may proceed -- recovering a messily-crashed connection with no restart.
    if (const std::uint64_t epoch = ep.mapping.requested_epoch();
        epoch != ep.acked_epoch) {
      pubsub.remove(ep);
      ep.mapping.drain_for_reconnect();
      ep.reset_connection(next_connection_id());
      ep.mapping.ack_epoch(epoch);
      ep.acked_epoch = epoch;
      ep.rebind_ring_views();  // local head/tail caches track the drained indices
    }

    const bool output_progress = flush_ring_output(ep);
    if (ep.close_requested || has_pending_output(ep)) {
      return output_progress;
    }

    const CommandExecutionOptions exec_options{
        .output_reserve_limit = config.max_output_buffer_bytes,
        .resp_version = &ep.resp_version,
        .connection_id = ep.connection_id,
        .script_engine = &script_engine,
        .luau_engine = &luau_engine,
        .wren_engine = &wren_engine,
        .tcl_engine = &tcl_engine,
        .upython_engine = &upython_engine,
        .quickjs_engine = &quickjs_engine};

    const auto record = ep.sq.peek();
    if (!record) {
      return false;
    }

    // Decide the protocol from the first 8 bytes: "GOBLINS!" -> the SBE binary wire,
    // else RESP. Decide RESP the moment the prefix diverges so a short inline RESP
    // command is never stalled waiting for a full magic that will not arrive.
    if (ep.wire_mode == detail::WireMode::undecided) {
      ep.inbuf.append(*record);
      ep.sq.pop();
#ifdef GOBLIN_HAS_SBE
      switch (match_goblin_magic(ep.inbuf)) {
        case MagicMatch::need_more:
          return true;  // matches so far but incomplete -> wait for the next record
        case MagicMatch::yes:
          ep.wire_mode = detail::WireMode::sbe;
          ep.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
          break;
        case MagicMatch::no:
          ep.wire_mode = detail::WireMode::resp2;
          break;
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
          ep.sq.pop();
        } else if (off > 0) {
          ep.inbuf.assign(record->data() + off, record->size() - off);
          ep.sq.pop();
        } else {
          // Incomplete first frame: buffer the whole record.
          ep.inbuf.assign(record->data(), record->size());
          ep.sq.pop();
        }
      } else
#endif
      {
        ep.inbuf.append(*record);
        ep.sq.pop();
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
      ep.inbuf.clear();
      while (ep.parser.pop_into(ep.fields)) {
        dispatch_resp_command(ep, store, pubsub, ep.fields, exec_options);
      }
      if (ep.parser.has_error()) {
        const std::size_t prior_size = ep.output.size();
        resp::append_error(ep.output, ep.parser.error());
        ep.record_reply(prior_size);
        ep.parser.clear();  // resync the byte stream after a protocol error
      }
    }

    (void)flush_ring_output(ep);
    return true;
  };

  // macOS has no core pinning; raise this busy-poll thread's priority so the scheduler
  // stops parking it mid-spin (a no-op elsewhere -- Linux uses taskset/isolcpus).
  ring::set_busy_poll_thread_realtime();

  // Idle spins between ring requests: running network_iteration (poll + active
  // expire) every empty loop is a p99 footgun for pure-ring clients. Service the
  // network every 64th idle pass; rings still starve it by design when busy.
  //
  // Pure spin (no park): a multi-ring server cannot block on one SQ without
  // missing the others. On Apple Silicon cpu_relax() is a compiler barrier (not
  // YIELD), so the spin does not volunteer deschedule; the client's adaptive
  // wait parks the other side of the handoff when the peer is stalled.
  unsigned idle_spins = 0;
  while (running.load(std::memory_order_relaxed)) {
    bool progressed = false;
    for (auto& endpoint : rings) {
      if (process_ring(*endpoint)) {
        progressed = true;
        break;  // restart from the highest-priority ring
      }
    }
    if (progressed) {
      idle_spins = 0;
      continue;  // a busy high-priority ring starves the lower ones -- by design
    }
    if ((++idle_spins & 63u) == 0) {
      network_iteration(0);  // sparse network pass while rings are quiet
    }
    ring::cpu_relax();
  }
  for (auto& endpoint : rings) {
    pubsub.remove(*endpoint);
  }
  return true;
}

}  // namespace

Server::Server(ServerConfig config, Store& store)
    : config_(std::move(config)), store_(store) {
  const std::size_t page = static_cast<std::size_t>(ring::page_size());
  if (config_.unsolicited_output_buffer_bytes == 0) {
    config_.unsolicited_output_buffer_bytes = page;
  } else if (config_.unsolicited_output_buffer_bytes <=
             std::numeric_limits<std::size_t>::max() - (page - 1)) {
    config_.unsolicited_output_buffer_bytes =
        ((config_.unsolicited_output_buffer_bytes + page - 1) / page) * page;
  } else {
    config_.unsolicited_output_buffer_bytes =
        std::numeric_limits<std::size_t>::max() & ~(page - 1);
  }
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

  auto listener = create_listener(config_);
  if (!listener) {
    return 1;
  }
  const int listener_fd = *listener;

  detail::PubSubRegistry pubsub;
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

  std::cout << "goblin-core listening on " << config_.bind_address << ':' << config_.port
            << '\n';

  // Keys expired per active-expiration sweep. A bounded batch keeps a mass
  // expiry from stalling the loop; when a sweep fills the batch more are likely
  // due, so the poll below returns immediately to sweep again.
  constexpr std::size_t kActiveExpireBudget = 1000;

  // One pass over the network: reap a finished background save, do a bounded
  // active-expiration sweep, then poll() (blocking up to max_timeout_ms) and
  // service ready clients and new connections. In ring mode this is invoked only
  // when every ring is empty, always with a timeout of 0 (never blocking).
  const auto network_iteration = [&](int max_timeout_ms) {
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
    if (!store_.ttl_empty()) {
      if (store_.active_expire(store_.now_ms(), kActiveExpireBudget) ==
          kActiveExpireBudget) {
        poll_timeout = 0;  // batch full -- more are likely due, sweep again now
      }
    }

    std::vector<pollfd> pollfds;
    pollfds.reserve(clients.size() + 1);
    pollfds.push_back(pollfd{.fd = listener_fd, .events = POLLIN, .revents = 0});
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
    for (std::size_t i = 0; i < clients.size(); ++i) {
      bool keep = !clients[i]->close_requested;
      auto& client = *clients[i];
      const auto revents = pollfds[i + 1].revents;

      if (keep && (revents & POLLOUT) != 0 && has_pending_output(client)) {
        keep = write_client(client, config_);
      }

      if (keep && !client.read_backpressured) {
        keep = process_buffered_commands(client, store_, pubsub, script_engine,
                                         luau_engine, wren_engine, tcl_engine,
                                         upython_engine, quickjs_engine, config_);
      }

      if (keep && (revents & POLLIN) != 0 && !client.read_backpressured) {
        keep = read_client(client, store_, pubsub, script_engine, luau_engine,
                           wren_engine, tcl_engine, upython_engine, quickjs_engine, config_);
      }

      if (keep && has_pending_output(client)) {
        keep = write_client(client, config_);
      }

      while (keep && !client.read_backpressured &&
             has_buffered_work(client)) {
        keep = process_buffered_commands(client, store_, pubsub, script_engine,
                                         luau_engine, wren_engine, tcl_engine,
                                         upython_engine, quickjs_engine, config_);
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
        close_fd(clients[index]->fd);
        clients.erase(clients.begin() + static_cast<long>(index));
      }
    }

    if ((pollfds[0].revents & POLLIN) != 0) {
      accept_clients(listener_fd, clients, config_);
    }
  };

  if (config_.rings.empty()) {
    // No rings: the ordinary event-driven server. poll() blocks, so an idle
    // server costs no CPU.
    while (running_) {
      network_iteration(1000);
    }
  } else if (!run_rings(config_, store_, running_, pubsub, script_engine, luau_engine,
                        wren_engine, tcl_engine, upython_engine, quickjs_engine,
                        network_iteration)) {
    close_fd(listener_fd);
    return 1;
  }

  for (const auto& client : clients) {
    pubsub.remove(*client);
    close_fd(client->fd);
  }
  close_fd(listener_fd);

  return 0;
}

}  // namespace goblin::core
