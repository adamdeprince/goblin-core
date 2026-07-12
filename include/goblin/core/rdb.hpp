#pragma once

// Redis RDB reader.
//
// Provenance: implemented clean-room from public RDB format descriptions for
// versions 6-11 (Redis 2.6 through 7.2.x). Cross-checked only against sources
// that are BSD-3-Clause: Redis <= 7.2.4 and Valkey. Do NOT read or transcribe
// Redis source from 7.4 onward (RSALv2/SSPL, incompatible with Apache-2.0);
// newer format details, if ever needed, come from descriptions or Valkey.
//
// Scope: sorted sets and lists are imported; strings, sets, and hashes are
// parsed and discarded; streams and modules abort the import (migrate those
// with the network import script). The floor is RDB v6 (Redis 2.6); the older
// zipmap hash encoding aborts with a "re-save under Redis >= 2.6" message.

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string_view>

#include "goblin/core/store.hpp"

namespace goblin::core::rdb {

class rdb_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// CRC64 (Jones polynomial, reflected) as used by Redis RDB checksums. Exposed
// for testing against the published vector; crc64("123456789") ==
// 0xe9c6d914c4b8d9ca.
[[nodiscard]] std::uint64_t crc64(std::string_view data) noexcept;

// Import every sorted set and list from a Redis RDB stream into `store`, which
// is cleared first. +/-inf scores clamp to +/-DBL_MAX; a member or list value
// larger than 64 KiB aborts. Throws rdb_error on malformed, unsupported, or
// out-of-range input.
SnapshotLoadStats import(Store& store, std::istream& in);

}  // namespace goblin::core::rdb
