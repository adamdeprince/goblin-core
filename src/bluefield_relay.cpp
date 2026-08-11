#include "bluefield_relay.hpp"

#include "bluefield_sbe.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace goblin::core::bluefield {

class EdgeRelay::Impl {
 public:
  Impl(std::string host, std::uint16_t port, std::uint64_t edge_id) {
    staged_.reserve(64);
    std::string error;
    auto aggregate = EdgeSbeClient::open(host, port, error);
    if (!aggregate) {
      throw std::runtime_error("connect aggregate SBE link: " + error);
    }
    auto publisher = EdgeSbeClient::open(std::move(host), port, error);
    if (!publisher) {
      throw std::runtime_error("connect publisher SBE link: " + error);
    }
    aggregate_ = std::move(aggregate);
    publisher_ = std::move(publisher);
    aggregate_->register_edge(edge_id, true);
    publisher_->register_edge(edge_id, false);
  }

  void subscription_changed(std::string_view name, bool pattern,
                            std::size_t subscribers) {
    if (subscribers > std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error(
          "BlueField subscriber count exceeds the host protocol limit");
    }
    auto& counts = pattern ? pattern_counts_ : channel_counts_;
    const auto found = counts.find(std::string(name));
    const bool registered = found != counts.end();

    if (subscribers == 0) {
      if (!registered) {
        return;
      }
      aggregate_->set_subscription(name, pattern, false);
      counts.erase(found);
      return;
    }

    if (!registered) {
      aggregate_->set_subscription(name, pattern, true);
      counts.emplace(name, subscribers);
      if (subscribers == 1) {
        return;
      }
    } else {
      found->second = subscribers;
    }

    aggregate_->set_subscription_weight(
        name, pattern, static_cast<std::uint32_t>(subscribers));
  }

  void stage_publish(std::uint64_t client_id, std::uint64_t reply_sequence,
                     long long local_deliveries, std::string_view channel,
                     std::string_view payload) {
    staged_.push_back(StagedPublish{.client_id = client_id,
                                    .reply_sequence = reply_sequence,
                                    .local_deliveries = local_deliveries,
                                    .channel = channel,
                                    .payload = payload});
  }

  [[nodiscard]] bool has_staged() const noexcept { return !staged_.empty(); }

  [[nodiscard]] bool flush_staged() {
    const bool had_staged = !staged_.empty();
    for (const auto& staged : staged_) {
      publisher_->enqueue_publish(staged.channel, staged.payload);
      pending_.push_back(PendingPublish{
          .client_id = staged.client_id,
          .reply_sequence = staged.reply_sequence,
          .local_deliveries = staged.local_deliveries});
    }
    staged_.clear();
    return had_staged;
  }

  void cancel(std::uint64_t client_id) noexcept {
    for (auto& staged : staged_) {
      if (staged.client_id == client_id) {
        staged.client_id = 0;
      }
    }
    for (auto& pending : pending_) {
      if (pending.client_id == client_id) {
        pending.client_id = 0;
      }
    }
  }

  [[nodiscard]] bool poll(detail::PubSubRegistry& pubsub,
                          std::vector<PublishCompletion>& completions) {
    bool progressed = false;
    for (std::size_t count = 0; count < 64; ++count) {
      auto message = aggregate_->try_read_pubsub();
      if (!message) {
        break;
      }
      progressed = true;
      if (message->kind == EdgePubSubKind::message ||
          message->kind == EdgePubSubKind::pattern_message) {
        (void)pubsub.publish(message->channel, message->payload);
      }
    }
    for (std::size_t count = 0; count < 64 && !pending_.empty(); ++count) {
      auto delivered = publisher_->try_read_publish_reply();
      if (!delivered) {
        break;
      }
      const auto pending = pending_.front();
      pending_.pop_front();
      progressed = true;
      if (pending.client_id != 0) {
        completions.push_back(
            PublishCompletion{.client_id = pending.client_id,
                              .reply_sequence = pending.reply_sequence,
                              .local_deliveries = pending.local_deliveries,
                              .upstream_deliveries = *delivered});
      }
    }
    return progressed;
  }

 private:
  struct StagedPublish {
    std::uint64_t client_id{0};
    std::uint64_t reply_sequence{0};
    long long local_deliveries{0};
    // Views alias a client's parsed RESP frame. A publishing client is marked
    // pending immediately, so its parser cannot compact or append before the
    // event loop flushes this staging vector later in the same iteration.
    std::string_view channel;
    std::string_view payload;
  };

  struct PendingPublish {
    std::uint64_t client_id{0};
    std::uint64_t reply_sequence{0};
    long long local_deliveries{0};
  };

  std::unique_ptr<EdgeSbeClient> aggregate_;
  std::unique_ptr<EdgeSbeClient> publisher_;
  std::unordered_map<std::string, std::size_t> channel_counts_;
  std::unordered_map<std::string, std::size_t> pattern_counts_;
  std::vector<StagedPublish> staged_;
  std::deque<PendingPublish> pending_;
};

EdgeRelay::EdgeRelay(std::string host, std::uint16_t port,
                     std::uint64_t edge_id)
    : impl_(std::make_unique<Impl>(std::move(host), port, edge_id)) {}

EdgeRelay::~EdgeRelay() = default;

detail::PubSubSubscriptionObserver EdgeRelay::observer() noexcept {
  return detail::PubSubSubscriptionObserver{
      .context = this,
      .changed = [](void* context, std::string_view name, bool pattern,
                    std::size_t subscribers) {
        static_cast<EdgeRelay*>(context)->subscription_changed(
            name, pattern, subscribers);
      }};
}

void EdgeRelay::stage_publish(std::uint64_t client_id,
                              std::uint64_t reply_sequence,
                              long long local_deliveries,
                              std::string_view channel,
                              std::string_view payload) {
  impl_->stage_publish(client_id, reply_sequence, local_deliveries, channel,
                       payload);
}

bool EdgeRelay::has_staged() const noexcept { return impl_->has_staged(); }

bool EdgeRelay::flush_staged() { return impl_->flush_staged(); }

void EdgeRelay::cancel(std::uint64_t client_id) noexcept {
  impl_->cancel(client_id);
}

bool EdgeRelay::poll(detail::PubSubRegistry& pubsub,
                     std::vector<PublishCompletion>& completions) {
  return impl_->poll(pubsub, completions);
}

void EdgeRelay::subscription_changed(std::string_view name, bool pattern,
                                     std::size_t subscribers) {
  impl_->subscription_changed(name, pattern, subscribers);
}

}  // namespace goblin::core::bluefield
