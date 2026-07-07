#pragma once

#include <cstddef>
#include <memory>

#include "goblin/core/zset_member_index.hpp"
#include "goblin/core/zset_member_storage.hpp"

namespace goblin::core {

// Hot member bytes + swiss lookup table. Shared across zsets with identical
// member/score sets (copy-on-write splits on per-key mutation).
struct ZSetMemberLayer {
  std::shared_ptr<ZSetMemberStorage> storage;
  ZSetMemberIndex members;

  ZSetMemberLayer(bool score_string_cache,
                  std::size_t member_chunk_bytes,
                  double member_index_growth);

  [[nodiscard]] std::shared_ptr<ZSetMemberLayer> clone(
      double member_index_growth) const;

  [[nodiscard]] std::shared_ptr<ZSetMemberLayer> clone_shallow(
      double member_index_growth) const;
};

}  // namespace goblin::core