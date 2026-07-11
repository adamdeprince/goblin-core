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
#endif
#include "goblin/core/store.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/wren_script.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
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

struct Client {
  int fd{-1};
  RespParser parser;
  std::string output;
  std::vector<std::string_view> fields;
  std::size_t output_offset{0};
  bool read_backpressured{false};
  bool close_after_write{false};
};

// Server-side state for one shared-memory ring: the mapping plus a RESP parser
// and reply buffer, mirroring a network Client but reading from the SQ and writing
// to the CQ instead of a socket.
// A ring/socket endpoint speaks RESP or the SBE binary wire, decided by the first
// 4 bytes (see goblin_protocol.hpp). `undecided` until enough bytes arrive.
enum class RingProto { undecided, resp, sbe };

struct RingEndpoint {
  ring::Mapping mapping;
  RingProto proto{RingProto::undecided};
  RespParser parser;
  std::string inbuf;   // raw accumulator: pre-decision bytes, then the SBE stream
  std::string output;
  std::vector<std::string_view> fields;
};

void close_fd(int fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
  }
}

[[nodiscard]] bool would_block() noexcept {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

[[nodiscard]] bool has_pending_output(const Client& client) noexcept {
  return client.output_offset < client.output.size();
}

[[nodiscard]] std::size_t pending_output_bytes(const Client& client) noexcept {
  return client.output.size() - client.output_offset;
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

void compact_output_if_needed(Client& client) {
  if (client.output_offset == 0) {
    return;
  }

  if (client.output_offset >= client.output.size()) {
    client.output.clear();
    client.output_offset = 0;
    return;
  }

  const auto remaining = client.output.size() - client.output_offset;
  if (client.output_offset >= kOutputCompactThreshold &&
      client.output_offset >= remaining) {
    client.output.erase(0, client.output_offset);
    client.output_offset = 0;
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
                    std::vector<Client>& clients,
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

    Client client{.fd = client_fd};
    if (config.initial_output_buffer_bytes > 0) {
      client.output.reserve(config.initial_output_buffer_bytes);
    }
    clients.push_back(std::move(client));
  }
}

[[nodiscard]] bool process_buffered_commands(Client& client,
                                             Store& store,
                                             ScriptEngine& script_engine,
                                             LuauEngine& luau_engine,
                                             WrenEngine& wren_engine,
                                             TclEngine& tcl_engine,
                                             UPythonEngine& upython_engine,
                                             QuickJsEngine& quickjs_engine,
                                             const ServerConfig& config) {
  compact_output_if_needed(client);
  update_read_backpressure(client, config);

  while (!client.read_backpressured) {
    if (!client.parser.pop_into(client.fields)) {
      break;
    }

    handle_command_into(
        store,
        client.fields,
        client.output,
        CommandExecutionOptions{
            .output_reserve_limit = config.max_output_buffer_bytes,
            .script_engine = &script_engine,
            .luau_engine = &luau_engine,
            .wren_engine = &wren_engine,
            .tcl_engine = &tcl_engine,
            .upython_engine = &upython_engine,
            .quickjs_engine = &quickjs_engine});
    compact_output_if_needed(client);
    update_read_backpressure(client, config);
  }

  if (client.parser.has_error()) {
    if (!client.close_after_write) {
      resp::append_error(client.output, client.parser.error());
    }
    client.close_after_write = true;
    update_read_backpressure(client, config);
  }

  return true;
}

[[nodiscard]] bool read_client(Client& client, Store& store,
                               ScriptEngine& script_engine,
                               LuauEngine& luau_engine,
                               WrenEngine& wren_engine,
                               TclEngine& tcl_engine,
                               UPythonEngine& upython_engine,
                               QuickJsEngine& quickjs_engine,
                               const ServerConfig& config) {
  char buffer[16 * 1024];

  for (;;) {
    if (client.read_backpressured) {
      return true;
    }

    const auto received = ::recv(client.fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      client.parser.append(std::string_view(buffer, static_cast<std::size_t>(received)));

      if (!process_buffered_commands(client, store, script_engine, luau_engine,
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
  while (has_pending_output(client)) {
    const auto pending = pending_output_bytes(client);
    const auto sent = ::send(
        client.fd,
        client.output.data() + client.output_offset,
        pending,
        0);
    if (sent > 0) {
      client.output_offset += static_cast<std::size_t>(sent);
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
                             std::atomic_bool& running, ScriptEngine& script_engine,
                             LuauEngine& luau_engine, WrenEngine& wren_engine,
                             TclEngine& tcl_engine, UPythonEngine& upython_engine,
                             QuickJsEngine& quickjs_engine,
                             NetFn&& network_iteration) {
  std::vector<RingEndpoint> rings;
  rings.reserve(config.rings.size());
  for (const auto& rc : config.rings) {
    const std::uint64_t cap = ring::capacity_for(rc.bytes);
    auto mapping = ring::Mapping::create(rc.path.c_str(), cap, cap);
    if (!mapping) {
      std::cerr << "goblin-core: failed to create ring " << rc.path << ": "
                << std::strerror(errno) << '\n';
      return false;
    }
    std::cout << "goblin-core: ring " << rc.path << " ready (" << cap
              << " bytes/direction)\n";
    rings.push_back(RingEndpoint{.mapping = std::move(*mapping)});
  }
  std::cout << "goblin-core: ring mode -- busy-polling " << rings.size()
            << " ring(s) ahead of the network (100% CPU by design)\n"
            << std::flush;

  const CommandExecutionOptions exec_options{
      .output_reserve_limit = config.max_output_buffer_bytes,
      .script_engine = &script_engine,
      .luau_engine = &luau_engine,
      .wren_engine = &wren_engine,
      .tcl_engine = &tcl_engine,
      .upython_engine = &upython_engine,
      .quickjs_engine = &quickjs_engine};

  // Drain one SQ record from `ep`, run whatever commands it completes, and push
  // the replies onto the CQ. Returns false when the ring's SQ was empty.
  const auto process_ring = [&](RingEndpoint& ep) -> bool {
    ring::Consumer sq = ep.mapping.sq_consumer();
    const auto record = sq.peek();
    if (!record) {
      return false;
    }
    ep.inbuf.append(*record);
    sq.pop();

    // Decide the protocol from the first 8 bytes: "GOBLINS!" -> the SBE binary wire,
    // else RESP. Decide RESP the moment the prefix diverges so a short inline RESP
    // command is never stalled waiting for a full magic that will not arrive.
    if (ep.proto == RingProto::undecided) {
#ifdef GOBLIN_HAS_SBE
      switch (match_goblin_magic(ep.inbuf)) {
        case MagicMatch::need_more:
          return true;  // matches so far but incomplete -> wait for the next record
        case MagicMatch::yes:
          ep.proto = RingProto::sbe;
          ep.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
          break;
        case MagicMatch::no:
          ep.proto = RingProto::resp;
          break;
      }
#else
      ep.proto = RingProto::resp;
#endif
    }

#ifdef GOBLIN_HAS_SBE
    if (ep.proto == RingProto::sbe) {
      // Frame and dispatch every complete SBE message in the accumulator; a partial
      // trailing message stays buffered for the next record.
      std::size_t off = 0;
      for (;;) {
        const std::size_t consumed = sbe_dispatch_one(
            store, std::string_view(ep.inbuf).substr(off), ep.output);
        if (consumed == 0) {
          break;
        }
        off += consumed;
      }
      if (off > 0) {
        ep.inbuf.erase(0, off);
      }
    } else
#endif
    {
      ep.parser.append(ep.inbuf);
      ep.inbuf.clear();
      while (ep.parser.pop_into(ep.fields)) {
        handle_command_into(store, ep.fields, ep.output, exec_options);
      }
      if (ep.parser.has_error()) {
        resp::append_error(ep.output, ep.parser.error());
        ep.parser.clear();  // resync the byte stream after a protocol error
      }
    }

    if (!ep.output.empty()) {
      ep.mapping.cq_producer().send(
          ep.output, [&] { return !running.load(std::memory_order_relaxed); });
      ep.output.clear();
    }
    return true;
  };

  while (running.load(std::memory_order_relaxed)) {
    bool progressed = false;
    for (RingEndpoint& ep : rings) {
      if (process_ring(ep)) {
        progressed = true;
        break;  // restart from the highest-priority ring
      }
    }
    if (progressed) {
      continue;  // a busy high-priority ring starves the lower ones -- by design
    }
    network_iteration(0);  // every ring empty -> now service the network
    ring::cpu_relax();     // PAUSE / yield: let remote cores publish into our pages
  }
  return true;
}

}  // namespace

Server::Server(ServerConfig config, Store& store)
    : config_(std::move(config)), store_(store) {
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

  std::vector<Client> clients;
  running_ = true;

  // One engine per interpreter for the process. Each holds its own script cache
  // and, lazily, its own VM -- no VM is created until the first script of that
  // kind runs, so a server that never scripts pays nothing here.
  ScriptEngine script_engine(store_);
  LuauEngine luau_engine(store_);
  WrenEngine wren_engine(store_);
  TclEngine tcl_engine(store_);
  UPythonEngine upython_engine(store_);
  QuickJsEngine quickjs_engine(store_);

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
    for (auto& client : clients) {
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
      bool keep = true;
      const auto revents = pollfds[i + 1].revents;

      if (keep && (revents & POLLOUT) != 0 && has_pending_output(clients[i])) {
        keep = write_client(clients[i], config_);
      }

      if (keep && !clients[i].read_backpressured) {
        keep = process_buffered_commands(clients[i], store_, script_engine,
                                         luau_engine, wren_engine, tcl_engine,
                                         upython_engine, quickjs_engine, config_);
      }

      if (keep && (revents & POLLIN) != 0 && !clients[i].read_backpressured) {
        keep = read_client(clients[i], store_, script_engine, luau_engine,
                           wren_engine, tcl_engine, upython_engine, quickjs_engine, config_);
      }

      if (keep && has_pending_output(clients[i])) {
        keep = write_client(clients[i], config_);
      }

      while (keep && !clients[i].read_backpressured &&
             clients[i].parser.has_queued_frames()) {
        keep = process_buffered_commands(clients[i], store_, script_engine,
                                         luau_engine, wren_engine, tcl_engine,
                                         upython_engine, quickjs_engine, config_);
        if (keep && has_pending_output(clients[i])) {
          keep = write_client(clients[i], config_);
        }
      }

      if (keep && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          !has_pending_output(clients[i])) {
        keep = false;
      }

      if (!keep) {
        close_client[i] = 1;
      }
    }

    for (std::size_t i = clients.size(); i > 0; --i) {
      const auto index = i - 1;
      if (close_client[index] != 0) {
        close_fd(clients[index].fd);
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
  } else if (!run_rings(config_, store_, running_, script_engine, luau_engine,
                        wren_engine, tcl_engine, upython_engine, quickjs_engine,
                        network_iteration)) {
    close_fd(listener_fd);
    return 1;
  }

  for (const auto& client : clients) {
    close_fd(client.fd);
  }
  close_fd(listener_fd);

  return 0;
}

}  // namespace goblin::core
