#pragma once

#include <string_view>

namespace goblin::core {

// Connection-owned services that an embedded script may reach when it re-enters
// the command dispatcher. The opaque context keeps the script engines independent
// of the server's internal Pub/Sub registry type.
struct NestedCommandDispatch {
  using Publish = long long (*)(void* context, std::string_view channel,
                                std::string_view payload);

  void* context{nullptr};
  Publish publish{nullptr};
};

}  // namespace goblin::core
