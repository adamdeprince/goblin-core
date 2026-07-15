#pragma once

#include <cstdint>

namespace goblin::core::resp {

// RESP is negotiated per connection with HELLO. New connections start in RESP2
// for compatibility with existing Redis clients.
enum class Version : std::uint8_t {
  resp2 = 2,
  resp3 = 3,
};

}  // namespace goblin::core::resp
