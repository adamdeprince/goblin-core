// Read-only diagnostic: exercise the actual packed integer hasher and the
// Swiss table's multiply-high bucket formula. No alternative hasher is used.
#include "goblin/core/packed_zset.hpp"

#include <cstdint>
#include <cstdio>
#include <unordered_set>

int main() {
  using Traits = goblin::core::detail::PackedIntegerTraits<std::int32_t>;
  constexpr std::size_t capacity = 1000000;
  const auto bucket = [](std::size_t hash) {
    return static_cast<std::size_t>(
        (static_cast<unsigned __int128>(hash) * capacity) >> 64);
  };
  std::printf("page_id\thash\tcapacity\tinitial_bucket\n");
  for (const auto id : {std::int32_t{10}, std::int32_t{26323569},
                        std::int32_t{84076787}, std::int32_t{2147483647}}) {
    const auto hash = Traits::Hash{}(id);
    std::printf("%d\t%zu\t%zu\t%zu\n", id, hash, capacity, bucket(hash));
  }
  std::unordered_set<std::size_t> buckets;
  for (std::int32_t id = 1; id <= 100000; ++id) {
    buckets.insert(bucket(Traits::Hash{}(id)));
  }
  std::printf("distinct_initial_buckets_for_100000_positive_ids=%zu\n", buckets.size());
}
