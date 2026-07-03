#include "goblin/core/server.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
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

[[nodiscard]] std::optional<int> create_listener(const ServerConfig& config) {
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
    set_tcp_nodelay(client_fd);

    Client client{.fd = client_fd};
    if (config.initial_output_buffer_bytes > 0) {
      client.output.reserve(config.initial_output_buffer_bytes);
    }
    clients.push_back(std::move(client));
  }
}

[[nodiscard]] bool process_buffered_commands(Client& client,
                                             Store& store,
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
        CommandExecutionOptions{.output_reserve_limit = config.max_output_buffer_bytes});
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

[[nodiscard]] bool read_client(Client& client, Store& store, const ServerConfig& config) {
  char buffer[16 * 1024];

  for (;;) {
    if (client.read_backpressured) {
      return true;
    }

    const auto received = ::recv(client.fd, buffer, sizeof(buffer), 0);
    if (received > 0) {
      client.parser.append(std::string_view(buffer, static_cast<std::size_t>(received)));

      if (!process_buffered_commands(client, store, config)) {
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

  std::vector<Client> clients;
  running_ = true;

  std::cout << "goblin-core listening on " << config_.bind_address << ':' << config_.port
            << '\n';

  while (running_) {
    std::vector<pollfd> pollfds;
    pollfds.reserve(clients.size() + 1);
    pollfds.push_back(pollfd{.fd = *listener, .events = POLLIN, .revents = 0});
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

    const int ready = ::poll(pollfds.data(), pollfds.size(), 1000);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "goblin-core: poll failed: " << std::strerror(errno) << '\n';
      break;
    }

    std::vector<unsigned char> close_client(clients.size(), 0);
    for (std::size_t i = 0; i < clients.size(); ++i) {
      bool keep = true;
      const auto revents = pollfds[i + 1].revents;

      if (keep && (revents & POLLOUT) != 0 && has_pending_output(clients[i])) {
        keep = write_client(clients[i], config_);
      }

      if (keep && !clients[i].read_backpressured) {
        keep = process_buffered_commands(clients[i], store_, config_);
      }

      if (keep && (revents & POLLIN) != 0 && !clients[i].read_backpressured) {
        keep = read_client(clients[i], store_, config_);
      }

      if (keep && has_pending_output(clients[i])) {
        keep = write_client(clients[i], config_);
      }

      while (keep && !clients[i].read_backpressured &&
             clients[i].parser.has_queued_frames()) {
        keep = process_buffered_commands(clients[i], store_, config_);
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
      accept_clients(*listener, clients, config_);
    }
  }

  for (const auto& client : clients) {
    close_fd(client.fd);
  }
  close_fd(*listener);

  return 0;
}

}  // namespace goblin::core
