#include "pubsub_listener.hpp"

#include "goblin/core/sbe_ring_client.hpp"
#include "pubsub.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace goblin::core::detail {

struct PubSubListenerRuntime::Impl {
  std::unique_ptr<SbeRingClient> ring_client;
  std::unique_ptr<SbeSocketClient> socket_client;
#ifdef GOBLIN_HAS_RDMA
  std::unique_ptr<SbeRdmaClient> rdma_client;
#endif
  std::string description;

  [[nodiscard]] std::optional<PubSubMessage> try_read_pubsub() {
    if (ring_client) {
      return ring_client->try_read_pubsub();
    }
    if (socket_client) {
      return socket_client->try_read_pubsub();
    }
#ifdef GOBLIN_HAS_RDMA
    return rdma_client->try_read_pubsub();
#else
    return std::nullopt;
#endif
  }

  void subscribe(std::string_view pattern) {
    const std::string_view patterns[]{pattern};
    if (ring_client) {
      if (ring_client->psubscribe(patterns).size() != 1) {
        throw std::runtime_error("upstream PSUBSCRIBE returned no acknowledgement");
      }
      return;
    }
    if (socket_client) {
      if (socket_client->psubscribe(patterns).size() != 1) {
        throw std::runtime_error("upstream PSUBSCRIBE returned no acknowledgement");
      }
      return;
    }
#ifdef GOBLIN_HAS_RDMA
    if (rdma_client->psubscribe(patterns).size() != 1) {
      throw std::runtime_error("upstream PSUBSCRIBE returned no acknowledgement");
    }
#endif
  }
};

PubSubListenerRuntime::PubSubListenerRuntime(
    const PubSubListenerConfig& config, std::string_view pattern)
    : impl_(std::make_unique<Impl>()) {
  if (const auto* listener = std::get_if<PubSubListenerRingConfig>(&config)) {
    auto client =
        SbeRingClient::open(listener->path.c_str(), std::chrono::seconds(5));
    if (!client) {
      throw std::runtime_error("could not connect to ring " + listener->path);
    }
    impl_->ring_client =
        std::make_unique<SbeRingClient>(std::move(*client));
    impl_->description = "ring " + listener->path;
  } else if (const auto* listener =
                 std::get_if<PubSubListenerUdsConfig>(&config)) {
    std::string error;
    auto client = SbeSocketClient::open(
        SbeSocketEndpoint::unix_domain(listener->path), std::chrono::seconds(5),
        SbeSocketClient::kDefaultBufferBytes, &error);
    if (!client) {
      throw std::runtime_error("could not connect to Unix socket " +
                               listener->path +
                               (error.empty() ? "" : ": " + error));
    }
    impl_->socket_client =
        std::make_unique<SbeSocketClient>(std::move(*client));
    impl_->description = "Unix socket " + listener->path;
  } else if (const auto* listener =
                 std::get_if<PubSubListenerTcpConfig>(&config)) {
    std::string error;
    auto client = SbeSocketClient::open(
        SbeSocketEndpoint::tcp(listener->address, listener->port),
        std::chrono::seconds(5), SbeSocketClient::kDefaultBufferBytes, &error);
    if (!client) {
      std::string message = "could not connect over TCP to " +
                            listener->address + ':' +
                            std::to_string(listener->port);
      if (!error.empty()) {
        message += ": " + error;
      }
      throw std::runtime_error(message);
    }
    impl_->socket_client =
        std::make_unique<SbeSocketClient>(std::move(*client));
    impl_->description = "TCP " + listener->address + ':' +
                         std::to_string(listener->port);
  } else {
#ifdef GOBLIN_HAS_RDMA
    const auto& rdma_listener = std::get<PubSubListenerRdmaConfig>(config);
    std::string error;
    auto client = SbeRdmaClient::open(
        rdma_listener.address, rdma_listener.port, rdma_listener.bytes,
        std::chrono::seconds(5),
        SbeRdmaClient::kDefaultBufferBytes, &error);
    if (!client) {
      std::string message = "could not connect via RDMA to " +
                            rdma_listener.address + ':' +
                            std::to_string(rdma_listener.port);
      if (!error.empty()) {
        message += ": " + error;
      }
      throw std::runtime_error(message);
    }
    impl_->rdma_client =
        std::make_unique<SbeRdmaClient>(std::move(*client));
    impl_->description = "RDMA " + rdma_listener.address + ':' +
                         std::to_string(rdma_listener.port);
#else
    (void)config;
    throw std::runtime_error("RDMA is unavailable in this build");
#endif
  }
  impl_->subscribe(pattern);
}

PubSubListenerRuntime::~PubSubListenerRuntime() = default;

bool PubSubListenerRuntime::rebroadcast_one(PubSubRegistry& pubsub) {
  auto message = impl_->try_read_pubsub();
  if (!message) {
    return false;
  }
  if (message->kind == PubSubKind::message ||
      message->kind == PubSubKind::pattern_message) {
    (void)pubsub.publish(message->channel, message->payload);
  }
  return true;
}

const std::string& PubSubListenerRuntime::description() const noexcept {
  return impl_->description;
}

}  // namespace goblin::core::detail
