#include "goblin/core/xlio_transport.hpp"

#if defined(GOBLIN_HAS_XLIO)

#include "goblin/core/ring_buffer.hpp"

#include <pthread.h>
#include <xlio_extra.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <utility>
#include <vector>

namespace goblin::core::xlio {
namespace detail {

namespace {

constexpr std::size_t kOutputFragmentBytes = 64U * 1024U;
constexpr std::size_t kInitialClientBufferBytes = 64U * 1024U;
constexpr std::size_t kInlineReceiveFragments = 16;

struct Environment {
  xlio_api_t* api{nullptr};

  ~Environment() {
    if (api != nullptr) {
      (void)api->xlio_exit();
    }
  }
};

std::mutex environment_mutex;
std::weak_ptr<Environment> shared_environment;

void assign_error(std::string& destination, std::string_view prefix,
                  int error_number = 0) noexcept {
  try {
    destination.assign(prefix);
    if (error_number != 0) {
      destination.append(": ");
      destination.append(std::strerror(error_number));
    }
  } catch (...) {
  }
}

[[nodiscard]] std::shared_ptr<Environment> acquire_environment(
    std::string& error) {
  std::lock_guard lock(environment_mutex);
  if (auto current = shared_environment.lock()) return current;

  auto environment = std::make_shared<Environment>();
  environment->api = xlio_get_api();
  if (environment->api == nullptr) {
    error = "XLIO Ultra API is unavailable; preload the pinned libxlio.so";
    return {};
  }
  if ((environment->api->cap_mask & XLIO_EXTRA_API_XLIO_ULTRA) == 0) {
    error = "loaded XLIO library does not expose the Ultra API";
    environment->api = nullptr;
    return {};
  }

  const xlio_init_attr attributes{
      .flags = 0,
      .memory_cb = nullptr,
      .memory_alloc = nullptr,
      .memory_free = nullptr,
  };
  if (environment->api->xlio_init_ex(&attributes) != 0) {
    const int saved_errno = errno;
    environment->api = nullptr;
    assign_error(error, "xlio_init_ex", saved_errno);
    return {};
  }
  shared_environment = environment;
  return environment;
}

enum class ContextKind : unsigned char {
  Listener,
  Connection,
};

}  // namespace

struct PollGroupState;

struct CallbackContext {
  ContextKind kind;
  PollGroupState* owner;
};

struct ReceiveFragment {
  void* data{nullptr};
  std::size_t size{0};
  xlio_buf* buffer{nullptr};
};

struct ConnectionState : CallbackContext {
  explicit ConnectionState(PollGroupState* group) noexcept
      : CallbackContext{ContextKind::Connection, group} {}

  xlio_socket_t socket{0};
  std::array<ReceiveFragment, kInlineReceiveFragments> inline_received{};
  std::size_t inline_head{0};
  std::size_t inline_count{0};
  std::vector<ReceiveFragment> overflow_received;
  std::size_t overflow_offset{0};
  bool established{false};
  bool peer_closed{false};
  bool destroy_pending{false};
  bool destroy_started{false};
  bool awaiting_termination{false};
  bool terminated{false};
  bool failed{false};
  bool attached{false};
  bool pending_accept{false};
  std::string error;

  [[nodiscard]] ReceiveFragment* front_received() noexcept {
    if (inline_count != 0) return &inline_received[inline_head];
    if (overflow_offset != overflow_received.size()) {
      return &overflow_received[overflow_offset];
    }
    return nullptr;
  }

  void enqueue_received(ReceiveFragment fragment) {
    if (overflow_offset == overflow_received.size() &&
        inline_count < inline_received.size()) {
      const std::size_t tail =
          (inline_head + inline_count) % inline_received.size();
      inline_received[tail] = fragment;
      ++inline_count;
      return;
    }
    overflow_received.push_back(fragment);
  }

  void pop_received() noexcept {
    if (inline_count != 0) {
      inline_head = (inline_head + 1) % inline_received.size();
      --inline_count;
      if (inline_count == 0) inline_head = 0;
      return;
    }
    if (overflow_offset != overflow_received.size()) ++overflow_offset;
    if (overflow_offset == overflow_received.size()) {
      overflow_received.clear();
      overflow_offset = 0;
    }
  }
};

struct ListenerState : CallbackContext {
  explicit ListenerState(std::shared_ptr<PollGroupState> poll_group) noexcept
      : CallbackContext{ContextKind::Listener, poll_group.get()},
        group(std::move(poll_group)) {}

  ~ListenerState();

  std::shared_ptr<PollGroupState> group;
  xlio_socket_t socket{0};
  std::deque<ConnectionState*> accepted;
  bool destroy_started{false};
  bool terminated{false};
  std::string error;
};

namespace {

void socket_event_callback(xlio_socket_t socket, uintptr_t userdata, int event,
                           int value) noexcept;
void socket_completion_callback(xlio_socket_t, uintptr_t, uintptr_t) noexcept {}
void socket_receive_callback(xlio_socket_t socket, uintptr_t userdata, void* data,
                             std::size_t size, xlio_buf* buffer) noexcept;
void socket_accept_callback(xlio_socket_t socket, xlio_socket_t parent,
                            uintptr_t parent_userdata) noexcept;

}  // namespace

struct PollGroupState {
  explicit PollGroupState(std::shared_ptr<Environment> environment) noexcept
      : environment(std::move(environment)) {}

  ~PollGroupState();

  [[nodiscard]] static std::shared_ptr<PollGroupState> create(
      std::string& error) {
    auto environment = acquire_environment(error);
    if (!environment) return {};

    auto state = std::make_shared<PollGroupState>(std::move(environment));
    const xlio_poll_group_attr attributes{
        .flags = 0,
        .socket_event_cb = socket_event_callback,
        .socket_comp_cb = socket_completion_callback,
        .socket_rx_cb = socket_receive_callback,
        .socket_accept_cb = socket_accept_callback,
    };
    if (state->environment->api->xlio_poll_group_create(&attributes,
                                                        &state->group) != 0) {
      assign_error(error, "xlio_poll_group_create", errno);
      return {};
    }
    return state;
  }

  void poll() noexcept {
    progressed = false;
    environment->api->xlio_poll_group_poll(group);
    destroy_deferred_sockets();
    destroy_pending_connections();
    reap_terminated_connections();
  }

  void defer_socket_destroy(xlio_socket_t socket) noexcept {
    if (deferred_destroy_count < deferred_destroy.size()) {
      deferred_destroy[deferred_destroy_count++] = socket;
      return;
    }
    // This is only reachable while already handling allocation/setup failure.
    // Group destruction remains the final owner of an unqueued socket.
    teardown_overflow = true;
  }

  void destroy_deferred_sockets() noexcept {
    for (std::size_t i = 0; i < deferred_destroy_count; ++i) {
      (void)environment->api->xlio_socket_destroy(deferred_destroy[i]);
    }
    deferred_destroy_count = 0;
  }

  void destroy_pending_connections() noexcept;

  void release_receive_buffers(ConnectionState& connection) noexcept {
    while (auto* fragment = connection.front_received()) {
      environment->api->xlio_poll_group_buf_free(group, fragment->buffer);
      connection.pop_received();
    }
  }

  void release_all_receive_buffers() noexcept {
    for (auto& connection : connections) {
      release_receive_buffers(*connection);
    }
  }

  void reap_terminated_connections() noexcept {
    std::erase_if(connections, [this](const auto& connection) {
      const bool reap = connection->terminated && !connection->attached &&
                        !connection->pending_accept;
      if (reap) release_receive_buffers(*connection);
      return reap;
    });
  }

  std::shared_ptr<Environment> environment;
  xlio_poll_group_t group{0};
  std::vector<std::unique_ptr<ConnectionState>> connections;
  std::array<xlio_socket_t, 64> deferred_destroy{};
  std::size_t deferred_destroy_count{0};
  bool teardown_overflow{false};
  bool progressed{false};
};

namespace {

void begin_destroy(ConnectionState& connection) noexcept {
  if (connection.socket == 0 || connection.destroy_started ||
      connection.terminated) {
    return;
  }
  connection.destroy_started = true;
  connection.destroy_pending = false;
  if (connection.owner->environment->api->xlio_socket_destroy(
          connection.socket) != 0) {
    connection.failed = true;
    assign_error(connection.error, "xlio_socket_destroy", errno);
  } else {
    connection.awaiting_termination = true;
  }
}

void socket_event_callback(xlio_socket_t socket, uintptr_t userdata, int event,
                           int value) noexcept {
  if (userdata == 0) return;
  auto& context = *reinterpret_cast<CallbackContext*>(userdata);
  context.owner->progressed = true;

  if (context.kind == ContextKind::Listener) {
    auto& listener = static_cast<ListenerState&>(context);
    // Newly accepted sockets initially inherit the parent's userdata. Ignore
    // their teardown events until accept_callback installs connection userdata.
    if (socket != listener.socket) return;
    if (event == XLIO_SOCKET_EVENT_TERMINATED) {
      listener.terminated = true;
    } else if (event == XLIO_SOCKET_EVENT_ERROR) {
      assign_error(listener.error, "XLIO listener", value);
    }
    return;
  }

  auto& connection = static_cast<ConnectionState&>(context);
  switch (event) {
    case XLIO_SOCKET_EVENT_ESTABLISHED:
      connection.established = true;
      break;
    case XLIO_SOCKET_EVENT_TERMINATED:
      connection.terminated = true;
      connection.awaiting_termination = false;
      connection.established = false;
      break;
    case XLIO_SOCKET_EVENT_CLOSED:
      connection.peer_closed = true;
      connection.established = false;
      break;
    case XLIO_SOCKET_EVENT_ERROR:
      connection.failed = true;
      connection.established = false;
      assign_error(connection.error, "XLIO socket", value);
      break;
    default:
      break;
  }
}

void socket_receive_callback(xlio_socket_t socket, uintptr_t userdata, void* data,
                             std::size_t size, xlio_buf* buffer) noexcept {
  if (userdata == 0) return;
  auto& context = *reinterpret_cast<CallbackContext*>(userdata);
  context.owner->progressed = true;
  if (context.kind != ContextKind::Connection) {
    context.owner->environment->api->xlio_socket_buf_free(socket, buffer);
    return;
  }

  auto& connection = static_cast<ConnectionState&>(context);
  try {
    connection.enqueue_received(
        ReceiveFragment{.data = data, .size = size, .buffer = buffer});
  } catch (...) {
    context.owner->environment->api->xlio_socket_buf_free(socket, buffer);
    connection.failed = true;
    assign_error(connection.error, "unable to queue XLIO receive buffer",
                 ENOMEM);
  }
}

void socket_accept_callback(xlio_socket_t socket, xlio_socket_t,
                            uintptr_t parent_userdata) noexcept {
  if (parent_userdata == 0) return;
  auto& context = *reinterpret_cast<CallbackContext*>(parent_userdata);
  if (context.kind != ContextKind::Listener) return;
  auto& listener = static_cast<ListenerState&>(context);
  auto& group = *listener.group;
  group.progressed = true;

  ConnectionState* raw = nullptr;
  bool registered = false;
  try {
    auto state = std::make_unique<ConnectionState>(&group);
    state->socket = socket;
    state->established = true;
    state->pending_accept = true;
    raw = state.get();
    group.connections.push_back(std::move(state));
    registered = true;

    if (group.environment->api->xlio_socket_update(
            socket, 0, reinterpret_cast<uintptr_t>(raw)) != 0) {
      raw->failed = true;
      raw->pending_accept = false;
      assign_error(raw->error, "xlio_socket_update", errno);
      // update failed, so the accepted socket still carries listener userdata.
      // Defer destruction until xlio_poll_group_poll() returns.
      raw->socket = 0;
      raw->terminated = true;
      group.defer_socket_destroy(socket);
      return;
    }
    int enabled = 1;
    if (group.environment->api->xlio_socket_setsockopt(
            socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
      raw->failed = true;
      raw->pending_accept = false;
      assign_error(raw->error, "XLIO TCP_NODELAY", errno);
      raw->destroy_pending = true;
      return;
    }
    listener.accepted.push_back(raw);
  } catch (...) {
    if (registered) {
      raw->failed = true;
      raw->pending_accept = false;
      raw->destroy_pending = true;
    } else {
      // XLIO currently forbids closing an accepted socket from this callback.
      group.defer_socket_destroy(socket);
    }
    assign_error(listener.error, "unable to allocate XLIO connection state",
                 ENOMEM);
  }
}

[[nodiscard]] bool resolve_address(std::string_view host, std::uint16_t port,
                                   int family, sockaddr_storage& storage,
                                   socklen_t& length, std::string& error) {
  addrinfo hints{};
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo* result = nullptr;
  const std::string host_storage(host);
  const std::string port_storage = std::to_string(port);
  const int status = ::getaddrinfo(host_storage.c_str(), port_storage.c_str(),
                                   &hints, &result);
  if (status != 0 || result == nullptr) {
    error = std::string("getaddrinfo: ") + ::gai_strerror(status);
    return false;
  }
  if (result->ai_addrlen > sizeof(storage)) {
    ::freeaddrinfo(result);
    error = "resolved XLIO address is too large";
    return false;
  }
  std::memcpy(&storage, result->ai_addr, result->ai_addrlen);
  length = static_cast<socklen_t>(result->ai_addrlen);
  ::freeaddrinfo(result);
  return true;
}

}  // namespace

void PollGroupState::destroy_pending_connections() noexcept {
  for (auto& connection : connections) {
    if (connection->destroy_pending) begin_destroy(*connection);
  }
}

PollGroupState::~PollGroupState() {
  release_all_receive_buffers();
  destroy_deferred_sockets();
  for (auto& connection : connections) begin_destroy(*connection);

  // XLIO guarantees a TERMINATED event after successful socket destruction.
  // Keep callback userdata alive until every such event has arrived.
  for (;;) {
    const bool awaiting_termination =
        std::any_of(connections.begin(), connections.end(), [](const auto& c) {
          return c->socket != 0 && c->awaiting_termination && !c->terminated;
        });
    if (!awaiting_termination) break;
    environment->api->xlio_poll_group_poll(group);
  }
  if (group != 0) {
    (void)environment->api->xlio_poll_group_destroy(group);
    group = 0;
  }
}

ListenerState::~ListenerState() {
  if (socket != 0 && !destroy_started && !terminated) {
    destroy_started = true;
    if (group->environment->api->xlio_socket_destroy(socket) == 0) {
      while (!terminated) group->poll();
    }
  }
}

}  // namespace detail

bool runtime_available() noexcept {
  const auto* api = xlio_get_api();
  return api != nullptr &&
         (api->cap_mask & XLIO_EXTRA_API_XLIO_ULTRA) != 0;
}

Connection::Connection(detail::ConnectionState* state) noexcept : state_(state) {
  if (state_ != nullptr) {
    state_->attached = true;
    state_->pending_accept = false;
  }
}

Connection::Connection(Connection&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this == &other) return *this;
  if (state_ != nullptr) {
    state_->owner->release_receive_buffers(*state_);
    detail::begin_destroy(*state_);
    state_->attached = false;
  }
  state_ = std::exchange(other.state_, nullptr);
  return *this;
}

Connection::~Connection() {
  if (state_ == nullptr) return;
  state_->owner->release_receive_buffers(*state_);
  detail::begin_destroy(*state_);
  state_->attached = false;
}

bool Connection::established() const noexcept {
  return state_ != nullptr && state_->established && !state_->destroy_started;
}

bool Connection::closed() const noexcept {
  return state_ == nullptr || state_->peer_closed;
}

bool Connection::terminated() const noexcept {
  return state_ == nullptr || state_->terminated;
}

bool Connection::failed() const noexcept {
  return state_ == nullptr || state_->failed;
}

std::string_view Connection::error() const noexcept {
  return state_ == nullptr ? std::string_view{} : std::string_view(state_->error);
}

std::optional<std::string_view> Connection::peek() noexcept {
  if (state_ == nullptr) return std::nullopt;
  const auto* fragment = state_->front_received();
  if (fragment == nullptr) return std::nullopt;
  return std::string_view(static_cast<const char*>(fragment->data),
                          fragment->size);
}

void Connection::pop() noexcept {
  if (state_ == nullptr) return;
  const auto* fragment = state_->front_received();
  if (fragment == nullptr) return;
  state_->owner->environment->api->xlio_poll_group_buf_free(
      state_->owner->group, fragment->buffer);
  state_->pop_received();
}

bool Connection::try_push(std::string_view bytes) noexcept {
  if (bytes.empty()) return true;
  if (!established()) return false;
  const xlio_socket_send_attr attributes{
      .flags = XLIO_SOCKET_SEND_FLAG_FLUSH | XLIO_SOCKET_SEND_FLAG_INLINE,
      .mkey = 0,
      .userdata_op = 0,
  };
  if (state_->owner->environment->api->xlio_socket_send(
          state_->socket, bytes.data(), bytes.size(), &attributes) == 0) {
    state_->owner->progressed = true;
    return true;
  }
  if (errno == ENOMEM || errno == EAGAIN || errno == EWOULDBLOCK) return false;
  state_->failed = true;
  detail::assign_error(state_->error, "xlio_socket_send", errno);
  return false;
}

std::size_t Connection::max_record_payload() const noexcept {
  return detail::kOutputFragmentBytes;
}

void Connection::close() noexcept {
  if (state_ != nullptr) detail::begin_destroy(*state_);
}

ServerListener::ServerListener(
    std::unique_ptr<detail::ListenerState> state) noexcept
    : state_(std::move(state)) {}

ServerListener::ServerListener(ServerListener&&) noexcept = default;
ServerListener& ServerListener::operator=(ServerListener&&) noexcept = default;
ServerListener::~ServerListener() = default;

std::unique_ptr<ServerListener> ServerListener::create(
    std::string_view bind_address, std::uint16_t port, std::string& error) {
  auto group = detail::PollGroupState::create(error);
  if (!group) return {};
  auto state = std::make_unique<detail::ListenerState>(group);

  sockaddr_storage address{};
  socklen_t address_length = 0;
  if (!detail::resolve_address(bind_address, port, AF_UNSPEC, address,
                               address_length, error)) {
    return {};
  }
  const xlio_socket_attr attributes{
      .flags = 0,
      .domain = address.ss_family,
      .group = group->group,
      .userdata_sq = reinterpret_cast<uintptr_t>(state.get()),
  };
  if (group->environment->api->xlio_socket_create(&attributes, &state->socket) !=
      0) {
    detail::assign_error(error, "xlio_socket_create", errno);
    return {};
  }
  int enabled = 1;
  if (group->environment->api->xlio_socket_setsockopt(
          state->socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) !=
          0 ||
      group->environment->api->xlio_socket_bind(
          state->socket, reinterpret_cast<const sockaddr*>(&address),
          address_length) != 0 ||
      group->environment->api->xlio_socket_listen(state->socket) != 0) {
    detail::assign_error(error, "XLIO listener setup", errno);
    return {};
  }
  return std::unique_ptr<ServerListener>(
      new ServerListener(std::move(state)));
}

ListenerPoll ServerListener::poll() noexcept {
  ListenerPoll result;
  if (!state_) return result;
  state_->group->poll();
  result.progressed = state_->group->progressed;
  if (!state_->accepted.empty()) {
    detail::ConnectionState* connection = state_->accepted.front();
    state_->accepted.pop_front();
    try {
      result.connection = std::unique_ptr<Connection>(new Connection(connection));
    } catch (...) {
      connection->pending_accept = false;
      connection->failed = true;
      detail::assign_error(connection->error,
                           "unable to allocate XLIO endpoint", ENOMEM);
      detail::begin_destroy(*connection);
      detail::assign_error(state_->error,
                           "unable to allocate XLIO endpoint", ENOMEM);
    }
    result.progressed = true;
  }
  return result;
}

std::string_view ServerListener::error() const noexcept {
  return state_ == nullptr ? std::string_view{} : std::string_view(state_->error);
}

ClientTransport::ClientTransport(
    std::shared_ptr<detail::PollGroupState> group,
    std::unique_ptr<Connection> connection) noexcept
    : group_(std::move(group)), connection_(std::move(connection)) {}

ClientTransport::ClientTransport(ClientTransport&&) noexcept = default;
ClientTransport& ClientTransport::operator=(ClientTransport&&) noexcept = default;
ClientTransport::~ClientTransport() = default;

std::optional<ClientTransport> ClientTransport::open(
    std::string_view host, std::uint16_t port, ms timeout,
    std::string_view local_address, std::string* error) {
  std::string local_error;
  auto group = detail::PollGroupState::create(local_error);
  if (!group) {
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }

  sockaddr_storage remote{};
  socklen_t remote_length = 0;
  if (!detail::resolve_address(host, port, AF_UNSPEC, remote, remote_length,
                               local_error)) {
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }

  auto state = std::make_unique<detail::ConnectionState>(group.get());
  detail::ConnectionState* raw = state.get();
  group->connections.push_back(std::move(state));
  const xlio_socket_attr attributes{
      .flags = 0,
      .domain = remote.ss_family,
      .group = group->group,
      .userdata_sq = reinterpret_cast<uintptr_t>(raw),
  };
  if (group->environment->api->xlio_socket_create(&attributes, &raw->socket) !=
      0) {
    detail::assign_error(local_error, "xlio_socket_create", errno);
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }
  int enabled = 1;
  if (group->environment->api->xlio_socket_setsockopt(
          raw->socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
    detail::assign_error(local_error, "XLIO TCP_NODELAY", errno);
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }

  if (!local_address.empty()) {
    sockaddr_storage local{};
    socklen_t local_length = 0;
    if (!detail::resolve_address(local_address, 0, remote.ss_family, local,
                                 local_length, local_error) ||
        group->environment->api->xlio_socket_bind(
            raw->socket, reinterpret_cast<const sockaddr*>(&local),
            local_length) != 0) {
      if (local_error.empty()) {
        detail::assign_error(local_error, "xlio_socket_bind", errno);
      }
      if (error != nullptr) *error = std::move(local_error);
      return std::nullopt;
    }
  }

  if (group->environment->api->xlio_socket_connect(
          raw->socket, reinterpret_cast<const sockaddr*>(&remote),
          remote_length) != 0) {
    detail::assign_error(local_error, "xlio_socket_connect", errno);
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!raw->established && !raw->failed && !raw->peer_closed) {
    group->poll();
    if (std::chrono::steady_clock::now() >= deadline) {
      local_error = "XLIO connection timed out";
      if (error != nullptr) *error = std::move(local_error);
      return std::nullopt;
    }
    ring::cpu_relax();
  }
  if (!raw->established) {
    local_error = raw->error.empty() ? "XLIO connection failed" : raw->error;
    if (error != nullptr) *error = std::move(local_error);
    return std::nullopt;
  }

  auto connection = std::unique_ptr<Connection>(new Connection(raw));
  return ClientTransport(std::move(group), std::move(connection));
}

std::optional<std::string_view> ClientTransport::peek() noexcept {
  if (!connection_) return std::nullopt;
  if (auto fragment = connection_->peek()) return fragment;
  poll();
  return connection_->peek();
}

void ClientTransport::pop() noexcept {
  if (connection_) connection_->pop();
}

void ClientTransport::wait_for_record() noexcept {
  poll();
  ring::cpu_relax();
}

void ClientTransport::poll() noexcept {
  if (group_) group_->poll();
}

bool ClientTransport::failed() const noexcept {
  return !connection_ || connection_->failed() || connection_->closed();
}

std::string_view ClientTransport::error() const noexcept {
  return connection_ ? connection_->error() : std::string_view{};
}

std::size_t ClientTransport::send_capacity() const noexcept {
  return detail::kInitialClientBufferBytes;
}

std::size_t ClientTransport::receive_capacity() const noexcept {
  return detail::kInitialClientBufferBytes;
}

std::size_t ClientTransport::max_message_bytes() const noexcept {
  return std::numeric_limits<std::uint32_t>::max();
}

std::size_t ClientTransport::buffer_size_hint() const noexcept {
  return detail::kInitialClientBufferBytes;
}

}  // namespace goblin::core::xlio

#endif  // GOBLIN_HAS_XLIO
