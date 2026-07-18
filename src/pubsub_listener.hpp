#pragma once

#include "goblin/core/server.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace goblin::core::detail {

class PubSubRegistry;

class PubSubListenerRuntime {
 public:
  PubSubListenerRuntime(const PubSubListenerConfig& config,
                        std::string_view pattern);
  ~PubSubListenerRuntime();

  PubSubListenerRuntime(const PubSubListenerRuntime&) = delete;
  PubSubListenerRuntime& operator=(const PubSubListenerRuntime&) = delete;

  [[nodiscard]] bool rebroadcast_one(PubSubRegistry& pubsub);
  [[nodiscard]] const std::string& description() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goblin::core::detail
