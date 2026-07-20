#pragma once

#include <string_view>

namespace goblin::core {

class Store;
struct Command;

// Connection-owned services that an embedded script may reach when it re-enters
// the command dispatcher. The opaque context keeps the script engines independent
// of the server's internal Pub/Sub registry type.
struct NestedCommandDispatch {
  using Publish = long long (*)(void* context, std::string_view channel,
                                std::string_view payload);
  using ReplicateWrite = void (*)(void* context, Store& store,
                                  const Command& command,
                                  std::string_view response) noexcept;

  void* context{nullptr};
  Publish publish{nullptr};
  void* replication_context{nullptr};
  ReplicateWrite replicate_write{nullptr};
  bool read_only{false};
};

}  // namespace goblin::core
