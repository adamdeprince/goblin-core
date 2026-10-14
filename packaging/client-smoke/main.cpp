#include <goblin/core/sbe_ring_client.hpp>
#include <type_traits>

int main() {
  static_assert(std::is_class_v<goblin::core::SbeSocketClient>);
  return 0;
}
