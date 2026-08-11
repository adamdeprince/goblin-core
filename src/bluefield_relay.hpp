#pragma once

#include "pubsub.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core::bluefield {

struct PublishCompletion {
  std::uint64_t client_id{0};
  std::uint64_t reply_sequence{0};
  long long local_deliveries{0};
  long long upstream_deliveries{0};
};

class EdgeRelay {
 public:
  EdgeRelay(std::string host, std::uint16_t port, std::uint64_t edge_id);
  ~EdgeRelay();

  EdgeRelay(const EdgeRelay&) = delete;
  EdgeRelay& operator=(const EdgeRelay&) = delete;
  EdgeRelay(EdgeRelay&&) = delete;
  EdgeRelay& operator=(EdgeRelay&&) = delete;

  [[nodiscard]] detail::PubSubSubscriptionObserver observer() noexcept;
  void stage_publish(std::uint64_t client_id, std::uint64_t reply_sequence,
                     long long local_deliveries, std::string_view channel,
                     std::string_view payload);
  [[nodiscard]] bool has_staged() const noexcept;
  [[nodiscard]] bool flush_staged();
  void cancel(std::uint64_t client_id) noexcept;
  [[nodiscard]] bool poll(detail::PubSubRegistry& pubsub,
                          std::vector<PublishCompletion>& completions);

 private:
  void subscription_changed(std::string_view name, bool pattern,
                            std::size_t subscribers);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goblin::core::bluefield
