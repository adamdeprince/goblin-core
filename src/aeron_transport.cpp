#include "goblin/core/aeron_transport.hpp"

#if defined(GOBLIN_HAS_AERON)

#include "goblin/core/ring_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <aeronc.h>
}

namespace goblin::core::aeron {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kFragmentLimit = 16;
constexpr auto kServerConnectTimeout = std::chrono::seconds(5);

[[nodiscard]] bool starts_with_aeron_uri(std::string_view value) noexcept {
  return value.starts_with("aeron:");
}

[[nodiscard]] std::optional<std::string_view> parameter_value(
    std::string_view uri, std::string_view key) noexcept {
  const auto query = uri.find('?');
  if (query == std::string_view::npos) return std::nullopt;
  std::string_view parameters = uri.substr(query + 1);
  while (!parameters.empty()) {
    const auto delimiter = parameters.find('|');
    const auto parameter = parameters.substr(0, delimiter);
    const auto equals = parameter.find('=');
    if (equals != std::string_view::npos &&
        parameter.substr(0, equals) == key) {
      return parameter.substr(equals + 1);
    }
    if (delimiter == std::string_view::npos) break;
    parameters.remove_prefix(delimiter + 1);
  }
  return std::nullopt;
}

[[nodiscard]] std::string append_parameter(std::string_view uri,
                                           std::string_view key,
                                           std::string_view value) {
  std::string result(uri);
  result.push_back(uri.find('?') == std::string_view::npos ? '?' : '|');
  result.append(key);
  result.push_back('=');
  result.append(value);
  return result;
}

[[nodiscard]] std::optional<std::string> response_mode_channel(
    std::string_view channel, std::string& error) {
  if (channel.empty() || !starts_with_aeron_uri(channel)) {
    error = "Aeron response channel must be a non-empty aeron: URI";
    return std::nullopt;
  }
  if (const auto mode = parameter_value(channel, "control-mode")) {
    if (*mode != "response") {
      error = "Aeron response channel control-mode must be response";
      return std::nullopt;
    }
    return std::string(channel);
  }
  return append_parameter(channel, "control-mode", "response");
}

[[nodiscard]] bool validate_channels(const ChannelConfig& channels,
                                     std::string& error) {
  if (channels.request_channel.empty() ||
      !starts_with_aeron_uri(channels.request_channel)) {
    error = "Aeron request channel must be a non-empty aeron: URI";
    return false;
  }
  if (parameter_value(channels.request_channel,
                      "response-correlation-id")) {
    error = "Aeron request channel must not set response-correlation-id";
    return false;
  }
  if (parameter_value(channels.response_channel,
                      "response-correlation-id")) {
    error = "Aeron response channel must not set response-correlation-id";
    return false;
  }
  return response_mode_channel(channels.response_channel, error).has_value();
}

void assign_noexcept(std::string& destination,
                     std::string_view value) noexcept {
  try {
    destination.assign(value);
  } catch (...) {
  }
}

[[nodiscard]] std::string aeron_error(std::string_view operation) {
  std::string result(operation);
  const char* detail = aeron_errmsg();
  if (detail != nullptr && *detail != '\0') {
    result.append(": ");
    result.append(detail);
  }
  return result;
}

template <class PollFn>
[[nodiscard]] bool await_async(PollFn&& poll, Clock::time_point deadline,
                               std::string_view operation,
                               std::string& error) {
  for (;;) {
    const int result = poll();
    if (result > 0) return true;
    if (result < 0) {
      error = aeron_error(operation);
      return false;
    }
    if (Clock::now() >= deadline) {
      error.assign(operation);
      error.append(" timed out");
      return false;
    }
    ring::cpu_relax();
  }
}

}  // namespace

namespace detail {

struct PeerState {
  aeron_image_t* image{nullptr};
  aeron_fragment_assembler_t* assembler{nullptr};
  aeron_async_add_publication_t* pending_publication{nullptr};
  aeron_publication_t* publication{nullptr};
  std::deque<std::string> records;
  std::int64_t correlation_id{0};
  std::size_t max_message_bytes{0};
  bool disconnected{false};
  bool failed{false};
  std::string error;
};

struct ServerState {
  ~ServerState();

  std::mutex mutex;
  aeron_context_t* context{nullptr};
  aeron_t* client{nullptr};
  aeron_async_add_subscription_t* pending_subscription{nullptr};
  aeron_subscription_t* subscription{nullptr};
  std::unordered_map<std::int64_t, std::shared_ptr<PeerState>> peers;
  std::vector<std::shared_ptr<PeerState>> peer_order;
  std::deque<std::shared_ptr<PeerState>> pending_connections;
  std::string response_mode_channel;
  std::int32_t response_stream_id{0};
  std::size_t next_peer{0};
  bool closing{false};
  bool failed{false};
  std::string error;
};

struct ClientState {
  ~ClientState();

  void fail(std::string_view message) noexcept {
    std::lock_guard lock(mutex);
    if (failed) return;
    failed = true;
    assign_noexcept(error, message);
  }

  std::mutex mutex;
  aeron_context_t* context{nullptr};
  aeron_t* client{nullptr};
  aeron_async_add_subscription_t* pending_subscription{nullptr};
  aeron_subscription_t* subscription{nullptr};
  aeron_async_add_publication_t* pending_publication{nullptr};
  aeron_publication_t* publication{nullptr};
  aeron_fragment_assembler_t* assembler{nullptr};
  std::deque<std::string> records;
  std::size_t max_message_bytes{0};
  bool failed{false};
  std::string error;
};

namespace {

void close_peer(ServerState& state, PeerState& peer) noexcept {
  peer.disconnected = true;
  peer.image = nullptr;
  if (peer.pending_publication != nullptr && state.client != nullptr) {
    (void)aeron_async_add_publication_cancel(
        state.client, peer.pending_publication);
    peer.pending_publication = nullptr;
  }
  if (peer.publication != nullptr) {
    (void)aeron_publication_close(peer.publication, nullptr, nullptr);
    peer.publication = nullptr;
  }
  if (peer.assembler != nullptr) {
    (void)aeron_fragment_assembler_delete(peer.assembler);
    peer.assembler = nullptr;
  }
}

void remove_peer(ServerState& state,
                 const std::shared_ptr<PeerState>& peer) noexcept {
  close_peer(state, *peer);
  const auto found = state.peers.find(peer->correlation_id);
  if (found != state.peers.end() && found->second == peer) {
    state.peers.erase(found);
  }
  std::erase(state.peer_order, peer);
  std::erase(state.pending_connections, peer);
  if (state.peer_order.empty() ||
      state.next_peer >= state.peer_order.size()) {
    state.next_peer = 0;
  }
}

void server_error_handler(void* clientd, int, const char* message) noexcept {
  auto& state = *static_cast<ServerState*>(clientd);
  std::lock_guard lock(state.mutex);
  if (state.closing || state.failed) return;
  state.failed = true;
  assign_noexcept(state.error,
                  message != nullptr ? std::string_view(message)
                                     : std::string_view("Aeron client error"));
}

void client_error_handler(void* clientd, int, const char* message) noexcept {
  auto& state = *static_cast<ClientState*>(clientd);
  state.fail(message != nullptr ? std::string_view(message)
                                : std::string_view("Aeron client error"));
}

void server_fragment_handler(void* clientd, const std::uint8_t* buffer,
                             std::size_t length, aeron_header_t*) noexcept {
  auto& peer = *static_cast<PeerState*>(clientd);
  try {
    peer.records.emplace_back(reinterpret_cast<const char*>(buffer), length);
  } catch (...) {
    peer.failed = true;
    assign_noexcept(peer.error,
                    "unable to queue a reassembled Aeron request");
  }
}

void client_fragment_handler(void* clientd, const std::uint8_t* buffer,
                             std::size_t length, aeron_header_t*) noexcept {
  auto& state = *static_cast<ClientState*>(clientd);
  try {
    state.records.emplace_back(reinterpret_cast<const char*>(buffer), length);
  } catch (...) {
    state.fail("unable to queue a reassembled Aeron response");
  }
}

void available_image(void* clientd, aeron_subscription_t*,
                     aeron_image_t* image) noexcept {
  auto& state = *static_cast<ServerState*>(clientd);
  aeron_image_constants_t constants{};
  if (aeron_image_constants(image, &constants) < 0) {
    std::lock_guard lock(state.mutex);
    state.failed = true;
    assign_noexcept(state.error, "query Aeron request image constants failed");
    return;
  }

  std::shared_ptr<PeerState> peer;
  try {
    peer = std::make_shared<PeerState>();
    peer->image = image;
    peer->correlation_id = constants.correlation_id;
    if (aeron_fragment_assembler_create(
            &peer->assembler, server_fragment_handler, peer.get()) < 0) {
      std::lock_guard lock(state.mutex);
      state.failed = true;
      assign_noexcept(state.error, aeron_error(
          "create Aeron request fragment assembler"));
      return;
    }

    const std::string response_channel = append_parameter(
        state.response_mode_channel, "response-correlation-id",
        std::to_string(constants.correlation_id));
    if (aeron_async_add_publication(
            &peer->pending_publication, state.client,
            response_channel.c_str(), state.response_stream_id) < 0) {
      (void)aeron_fragment_assembler_delete(peer->assembler);
      peer->assembler = nullptr;
      std::lock_guard lock(state.mutex);
      state.failed = true;
      assign_noexcept(state.error,
                      aeron_error("add Aeron response publication"));
      return;
    }

    std::lock_guard lock(state.mutex);
    if (state.closing) {
      close_peer(state, *peer);
      return;
    }
    if (state.peers.contains(constants.correlation_id)) {
      close_peer(state, *peer);
      state.failed = true;
      assign_noexcept(state.error,
                      "duplicate Aeron request image correlation id");
      return;
    }
    state.peers.emplace(constants.correlation_id, peer);
    try {
      state.peer_order.push_back(peer);
      state.pending_connections.push_back(peer);
    } catch (...) {
      remove_peer(state, peer);
      throw;
    }
  } catch (const std::exception& exception) {
    std::lock_guard lock(state.mutex);
    if (peer) remove_peer(state, peer);
    state.failed = true;
    assign_noexcept(state.error, exception.what());
  } catch (...) {
    std::lock_guard lock(state.mutex);
    if (peer) remove_peer(state, peer);
    state.failed = true;
    assign_noexcept(state.error, "unknown Aeron image setup failure");
  }
}

void unavailable_image(void* clientd, aeron_subscription_t*,
                       aeron_image_t* image) noexcept {
  auto& state = *static_cast<ServerState*>(clientd);
  aeron_image_constants_t constants{};
  if (aeron_image_constants(image, &constants) < 0) return;

  std::lock_guard lock(state.mutex);
  if (state.closing) return;
  const auto found = state.peers.find(constants.correlation_id);
  if (found == state.peers.end()) return;
  const auto peer = found->second;
  remove_peer(state, peer);
}

}  // namespace

ServerState::~ServerState() {
  {
    std::lock_guard lock(mutex);
    closing = true;
  }
  if (pending_subscription != nullptr && client != nullptr) {
    (void)aeron_async_add_subscription_cancel(client, pending_subscription);
    pending_subscription = nullptr;
  }
  if (subscription != nullptr) {
    (void)aeron_subscription_close(subscription, nullptr, nullptr);
    subscription = nullptr;
  }
  {
    std::lock_guard lock(mutex);
    for (auto& [correlation_id, peer] : peers) {
      (void)correlation_id;
      close_peer(*this, *peer);
    }
    peers.clear();
    peer_order.clear();
    pending_connections.clear();
  }
  if (client != nullptr) {
    (void)aeron_close(client);
    client = nullptr;
  }
  if (context != nullptr) {
    (void)aeron_context_close(context);
    context = nullptr;
  }
}

ClientState::~ClientState() {
  if (assembler != nullptr) {
    (void)aeron_fragment_assembler_delete(assembler);
    assembler = nullptr;
  }
  if (pending_publication != nullptr && client != nullptr) {
    (void)aeron_async_add_publication_cancel(client, pending_publication);
    pending_publication = nullptr;
  }
  if (publication != nullptr) {
    (void)aeron_publication_close(publication, nullptr, nullptr);
    publication = nullptr;
  }
  if (pending_subscription != nullptr && client != nullptr) {
    (void)aeron_async_add_subscription_cancel(client, pending_subscription);
    pending_subscription = nullptr;
  }
  if (subscription != nullptr) {
    (void)aeron_subscription_close(subscription, nullptr, nullptr);
    subscription = nullptr;
  }
  if (client != nullptr) {
    (void)aeron_close(client);
    client = nullptr;
  }
  if (context != nullptr) {
    (void)aeron_context_close(context);
    context = nullptr;
  }
}

}  // namespace detail

ChannelConfig ChannelConfig::udp(
    std::string_view request_endpoint_or_channel,
    std::int32_t request_stream_id,
    std::string_view response_control_endpoint_or_channel,
    std::int32_t response_stream_id) {
  ChannelConfig result;
  if (starts_with_aeron_uri(request_endpoint_or_channel)) {
    result.request_channel.assign(request_endpoint_or_channel);
  } else {
    result.request_channel.assign("aeron:udp?endpoint=");
    result.request_channel.append(request_endpoint_or_channel);
  }
  result.request_stream_id = request_stream_id;
  if (starts_with_aeron_uri(response_control_endpoint_or_channel)) {
    result.response_channel.assign(response_control_endpoint_or_channel);
  } else {
    result.response_channel.assign("aeron:udp?control=");
    result.response_channel.append(response_control_endpoint_or_channel);
  }
  result.response_stream_id = response_stream_id;
  return result;
}

ChannelConfig ChannelConfig::ipc(std::int32_t request_stream_id,
                                 std::int32_t response_stream_id) {
  return ChannelConfig{.request_channel = "aeron:ipc",
                       .request_stream_id = request_stream_id,
                       .response_channel = "aeron:ipc",
                       .response_stream_id = response_stream_id};
}

Connection::Connection(std::shared_ptr<detail::ServerState> server,
                       std::shared_ptr<detail::PeerState> peer) noexcept
    : server_(std::move(server)), peer_(std::move(peer)) {}

Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() { disconnect(); }

bool Connection::established() const noexcept {
  if (!server_ || !peer_) return false;
  std::lock_guard lock(server_->mutex);
  return !peer_->disconnected && !peer_->failed &&
         peer_->publication != nullptr &&
         aeron_publication_is_connected(peer_->publication);
}

bool Connection::disconnected() const noexcept {
  if (!server_ || !peer_) return true;
  std::lock_guard lock(server_->mutex);
  return peer_->disconnected;
}

bool Connection::failed() const noexcept {
  if (!server_ || !peer_) return true;
  std::lock_guard lock(server_->mutex);
  return peer_->failed;
}

std::string_view Connection::error() const noexcept {
  if (!server_ || !peer_) return {};
  std::lock_guard lock(server_->mutex);
  return peer_->error;
}

std::optional<std::string_view> Connection::peek() noexcept {
  if (!peer_ || peer_->records.empty()) return std::nullopt;
  return peer_->records.front();
}

void Connection::pop() noexcept {
  if (peer_ && !peer_->records.empty()) peer_->records.pop_front();
}

bool Connection::try_push(std::string_view bytes) noexcept {
  if (!server_ || !peer_) return false;
  std::lock_guard lock(server_->mutex);
  if (peer_->publication == nullptr || peer_->disconnected || peer_->failed) {
    return false;
  }
  if (bytes.size() > peer_->max_message_bytes) {
    peer_->failed = true;
    assign_noexcept(peer_->error,
                    "Goblin response exceeds Aeron's maximum message length");
    return false;
  }
  const auto result = aeron_publication_offer(
      peer_->publication,
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
      nullptr, nullptr);
  if (result > 0) return true;
  if (result == AERON_PUBLICATION_BACK_PRESSURED ||
      result == AERON_PUBLICATION_ADMIN_ACTION ||
      result == AERON_PUBLICATION_NOT_CONNECTED) {
    return false;
  }
  peer_->failed = true;
  assign_noexcept(peer_->error, aeron_error("offer Aeron response"));
  return false;
}

std::size_t Connection::max_record_payload() const noexcept {
  if (!server_ || !peer_) return 0;
  std::lock_guard lock(server_->mutex);
  return peer_->max_message_bytes;
}

void Connection::disconnect() noexcept {
  if (!server_ || !peer_) return;
  std::lock_guard lock(server_->mutex);
  if (!peer_->disconnected) detail::remove_peer(*server_, peer_);
}

ServerListener::ServerListener(
    std::shared_ptr<detail::ServerState> state) noexcept
    : state_(std::move(state)) {}

ServerListener::ServerListener(ServerListener&&) noexcept = default;
ServerListener& ServerListener::operator=(ServerListener&&) noexcept = default;
ServerListener::~ServerListener() = default;

std::unique_ptr<ServerListener> ServerListener::create(
    const ChannelConfig& channels, std::string_view aeron_directory,
    std::string& error) {
  error.clear();
  if (!validate_channels(channels, error)) return nullptr;
  auto response_channel = response_mode_channel(channels.response_channel,
                                                error);
  if (!response_channel) return nullptr;

  auto state = std::make_shared<detail::ServerState>();
  state->response_mode_channel = std::move(*response_channel);
  state->response_stream_id = channels.response_stream_id;
  if (aeron_context_init(&state->context) < 0) {
    error = aeron_error("initialize Aeron server context");
    return nullptr;
  }
  if ((!aeron_directory.empty() &&
       aeron_context_set_dir(state->context,
                             std::string(aeron_directory).c_str()) < 0) ||
      aeron_context_set_client_name(state->context, "goblin-core-server") < 0 ||
      aeron_context_set_driver_timeout_ms(
          state->context,
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  kServerConnectTimeout).count())) < 0 ||
      aeron_context_set_error_handler(state->context,
                                      detail::server_error_handler,
                                      state.get()) < 0) {
    error = aeron_error("configure Aeron server context");
    return nullptr;
  }
  if (aeron_init(&state->client, state->context) < 0 ||
      aeron_start(state->client) < 0) {
    error = aeron_error("connect Aeron server client to Media Driver");
    return nullptr;
  }
  if (aeron_async_add_subscription(
          &state->pending_subscription, state->client,
          channels.request_channel.c_str(), channels.request_stream_id,
          detail::available_image, state.get(), detail::unavailable_image,
          state.get()) < 0) {
    error = aeron_error("add Aeron request subscription");
    return nullptr;
  }
  const auto deadline = Clock::now() + kServerConnectTimeout;
  if (!await_async(
          [&] {
            return aeron_async_add_subscription_poll(
                &state->subscription, state->pending_subscription);
          },
          deadline, "add Aeron request subscription", error)) {
    return nullptr;
  }
  state->pending_subscription = nullptr;
  return std::unique_ptr<ServerListener>(
      new ServerListener(std::move(state)));
}

ListenerPoll ServerListener::poll() noexcept {
  ListenerPoll result;
  if (!state_) return result;
  std::lock_guard lock(state_->mutex);

  if (!state_->pending_connections.empty()) {
    auto peer = std::move(state_->pending_connections.front());
    state_->pending_connections.pop_front();
    try {
      result.connection = std::unique_ptr<Connection>(
          new Connection(state_, std::move(peer)));
    } catch (...) {
      state_->failed = true;
      assign_noexcept(state_->error,
                      "unable to allocate Aeron connection wrapper");
    }
    result.progressed = true;
  }

  auto& peers = state_->peer_order;
  if (peers.empty()) {
    state_->next_peer = 0;
    return result;
  }
  state_->next_peer %= peers.size();
  for (std::size_t offset = 0; offset < peers.size(); ++offset) {
    const std::size_t index = (state_->next_peer + offset) % peers.size();
    auto& peer = *peers[index];
    if (peer.disconnected || peer.failed) continue;

    if (peer.pending_publication != nullptr) {
      const int completed = aeron_async_add_publication_poll(
          &peer.publication, peer.pending_publication);
      if (completed < 0) {
        peer.failed = true;
        assign_noexcept(peer.error,
                        aeron_error("add Aeron response publication"));
        result.progressed = true;
        continue;
      }
      if (completed > 0) {
        peer.pending_publication = nullptr;
        aeron_publication_constants_t constants{};
        if (aeron_publication_constants(peer.publication, &constants) < 0) {
          peer.failed = true;
          assign_noexcept(peer.error,
                          aeron_error("query Aeron publication constants"));
        } else {
          peer.max_message_bytes = constants.max_message_length;
        }
        result.progressed = true;
      }
    }

    // Do not advance the image while its prior complete record is waiting for
    // the server. Leaving data in Aeron's term buffer propagates backpressure
    // instead of turning a fast publisher into an unbounded heap queue here.
    if (peer.records.empty() && peer.image != nullptr &&
        peer.assembler != nullptr) {
      const int fragments = aeron_image_poll(
          peer.image, aeron_fragment_assembler_handler, peer.assembler,
          1);
      if (fragments < 0) {
        peer.failed = true;
        assign_noexcept(peer.error, aeron_error("poll Aeron request image"));
        result.progressed = true;
      } else if (fragments > 0) {
        result.progressed = true;
        state_->next_peer = (index + 1) % peers.size();
        break;
      }
    }
  }
  return result;
}

std::string_view ServerListener::error() const noexcept {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  return state_->error;
}

ClientTransport::ClientTransport(
    std::shared_ptr<detail::ClientState> state,
    std::size_t buffer_size) noexcept
    : state_(std::move(state)), buffer_size_(buffer_size) {}

ClientTransport::ClientTransport(ClientTransport&&) noexcept = default;
ClientTransport& ClientTransport::operator=(ClientTransport&&) noexcept =
    default;
ClientTransport::~ClientTransport() = default;

std::optional<ClientTransport> ClientTransport::open(
    const ChannelConfig& channels, ms timeout, std::size_t buffer_size,
    std::string_view aeron_directory, std::string* error_out) {
  std::string error;
  if (!validate_channels(channels, error)) {
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  auto response_channel = response_mode_channel(channels.response_channel,
                                                error);
  if (!response_channel) {
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  const auto deadline = Clock::now() + timeout;
  auto state = std::make_shared<detail::ClientState>();
  if (aeron_context_init(&state->context) < 0) {
    error = aeron_error("initialize Aeron client context");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  const auto timeout_count = std::max<std::int64_t>(1, timeout.count());
  if ((!aeron_directory.empty() &&
       aeron_context_set_dir(state->context,
                             std::string(aeron_directory).c_str()) < 0) ||
      aeron_context_set_client_name(state->context, "goblin-core-client") < 0 ||
      aeron_context_set_driver_timeout_ms(
          state->context, static_cast<std::uint64_t>(timeout_count)) < 0 ||
      aeron_context_set_error_handler(state->context,
                                      detail::client_error_handler,
                                      state.get()) < 0) {
    error = aeron_error("configure Aeron client context");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  if (aeron_init(&state->client, state->context) < 0 ||
      aeron_start(state->client) < 0) {
    error = aeron_error("connect Aeron client to Media Driver");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  if (aeron_async_add_subscription(
          &state->pending_subscription, state->client,
          response_channel->c_str(), channels.response_stream_id,
          nullptr, nullptr, nullptr, nullptr) < 0) {
    error = aeron_error("add Aeron response subscription");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  if (!await_async(
          [&] {
            return aeron_async_add_subscription_poll(
                &state->subscription, state->pending_subscription);
          },
          deadline, "add Aeron response subscription", error)) {
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  state->pending_subscription = nullptr;

  aeron_subscription_constants_t subscription_constants{};
  if (aeron_subscription_constants(state->subscription,
                                   &subscription_constants) < 0) {
    error = aeron_error("query Aeron response subscription constants");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  const std::string request_channel = append_parameter(
      channels.request_channel, "response-correlation-id",
      std::to_string(subscription_constants.registration_id));
  if (aeron_async_add_publication(
          &state->pending_publication, state->client,
          request_channel.c_str(), channels.request_stream_id) < 0) {
    error = aeron_error("add Aeron request publication");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  if (!await_async(
          [&] {
            return aeron_async_add_publication_poll(
                &state->publication, state->pending_publication);
          },
          deadline, "add Aeron request publication", error)) {
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  state->pending_publication = nullptr;

  aeron_publication_constants_t publication_constants{};
  if (aeron_publication_constants(state->publication,
                                  &publication_constants) < 0) {
    error = aeron_error("query Aeron request publication constants");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }
  state->max_message_bytes = publication_constants.max_message_length;
  if (aeron_fragment_assembler_create(
          &state->assembler, detail::client_fragment_handler,
          state.get()) < 0) {
    error = aeron_error("create Aeron response fragment assembler");
    if (error_out) *error_out = std::move(error);
    return std::nullopt;
  }

  while (!(aeron_publication_is_connected(state->publication) &&
           aeron_subscription_is_connected(state->subscription))) {
    {
      std::lock_guard lock(state->mutex);
      if (state->failed) {
        error = state->error;
        if (error_out) *error_out = std::move(error);
        return std::nullopt;
      }
    }
    if (Clock::now() >= deadline) {
      error = "Aeron request/response channel connection timed out";
      if (error_out) *error_out = std::move(error);
      return std::nullopt;
    }
    ring::cpu_relax();
  }
  if (error_out) error_out->clear();
  return ClientTransport(std::move(state), buffer_size);
}

std::optional<ClientTransport> ClientTransport::open(
    std::string_view request_channel, std::int32_t request_stream_id,
    std::string_view response_channel, std::int32_t response_stream_id,
    ms timeout, std::size_t buffer_size, std::string_view aeron_directory,
    std::string* error) {
  return open(ChannelConfig{.request_channel = std::string(request_channel),
                            .request_stream_id = request_stream_id,
                            .response_channel = std::string(response_channel),
                            .response_stream_id = response_stream_id},
              timeout, buffer_size, aeron_directory, error);
}

bool ClientTransport::send_one(std::string_view bytes) noexcept {
  if (!state_ || state_->publication == nullptr) return false;
  if (bytes.size() > state_->max_message_bytes) {
    state_->fail("Goblin request exceeds Aeron's maximum message length");
    return false;
  }
  const auto result = aeron_publication_offer(
      state_->publication,
      reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
      nullptr, nullptr);
  if (result > 0) return true;
  if (result == AERON_PUBLICATION_BACK_PRESSURED ||
      result == AERON_PUBLICATION_ADMIN_ACTION ||
      result == AERON_PUBLICATION_NOT_CONNECTED) {
    return false;
  }
  state_->fail(aeron_error("offer Aeron request"));
  return false;
}

std::optional<std::string_view> ClientTransport::peek() noexcept {
  if (!state_) return std::nullopt;
  poll();
  if (state_->records.empty()) return std::nullopt;
  return state_->records.front();
}

void ClientTransport::pop() noexcept {
  if (state_ && !state_->records.empty()) state_->records.pop_front();
}

void ClientTransport::wait_for_record() noexcept {
  poll();
  ring::cpu_relax();
}

void ClientTransport::poll() noexcept {
  if (!state_ || state_->subscription == nullptr || failed()) return;
  const int fragments = aeron_subscription_poll(
      state_->subscription, aeron_fragment_assembler_handler,
      state_->assembler, kFragmentLimit);
  if (fragments < 0) {
    state_->fail(aeron_error("poll Aeron response subscription"));
  }
}

bool ClientTransport::failed() const noexcept {
  if (!state_) return true;
  std::lock_guard lock(state_->mutex);
  return state_->failed || state_->publication == nullptr ||
         aeron_publication_is_closed(state_->publication) ||
         state_->subscription == nullptr ||
         aeron_subscription_is_closed(state_->subscription);
}

std::string_view ClientTransport::error() const noexcept {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  return state_->error;
}

std::size_t ClientTransport::send_capacity() const noexcept {
  return buffer_size_;
}

std::size_t ClientTransport::receive_capacity() const noexcept {
  return buffer_size_;
}

std::size_t ClientTransport::max_message_bytes() const noexcept {
  return state_ ? state_->max_message_bytes : 0;
}

std::size_t ClientTransport::buffer_size_hint() const noexcept {
  return buffer_size_;
}

}  // namespace goblin::core::aeron

#endif  // GOBLIN_HAS_AERON
