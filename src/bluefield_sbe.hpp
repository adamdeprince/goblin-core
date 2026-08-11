#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace goblin::core::bluefield {

enum class EdgePubSubKind : std::uint8_t {
  message = 0,
  pattern_message = 1,
  subscribe = 2,
  pattern_subscribe = 3,
  unsubscribe = 4,
  pattern_unsubscribe = 5,
};

// The views remain valid until the next call on the same EdgeSbeClient.
struct EdgePubSubMessage {
  EdgePubSubKind kind{EdgePubSubKind::message};
  std::uint32_t subscription_count{0};
  std::string_view pattern;
  std::string_view channel;
  std::string_view payload;
};

// Purpose-built host link for the BlueField hot path. It includes only the six
// SBE message shapes the edge needs, instead of the full command client and its
// shared-memory/RDMA transports.
class EdgeSbeClient {
 public:
  [[nodiscard]] static std::unique_ptr<EdgeSbeClient> open(
      std::string host, std::uint16_t port, std::string& error);

  ~EdgeSbeClient();

  EdgeSbeClient(const EdgeSbeClient&) = delete;
  EdgeSbeClient& operator=(const EdgeSbeClient&) = delete;
  EdgeSbeClient(EdgeSbeClient&&) = delete;
  EdgeSbeClient& operator=(EdgeSbeClient&&) = delete;

  void register_edge(std::uint64_t edge_id, bool aggregate);
  void set_subscription(std::string_view name, bool pattern, bool subscribe);
  void set_subscription_weight(std::string_view name, bool pattern,
                               std::uint32_t subscribers);

  void enqueue_publish(std::string_view channel, std::string_view payload);
  [[nodiscard]] std::optional<long long> try_read_publish_reply();
  [[nodiscard]] std::optional<EdgePubSubMessage> try_read_pubsub();

 private:
  class Impl;
  explicit EdgeSbeClient(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace goblin::core::bluefield
