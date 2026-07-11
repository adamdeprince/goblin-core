#pragma once

// The protocol selector shared by the server parser and every first-party client.
//
// The first 8 bytes a peer writes on a socket or a ring SQ choose the wire protocol
// for the life of that connection:
//   * "GOBLINS!" -> the SBE binary wire (sbe/goblin_sbe.xml): length-prefixed SBE
//                   frames, decoded zero-copy and dispatched through a templateId
//                   jump table.
//   * anything else -> RESP (Redis serialization), unchanged.
//
// The magic is 8 bytes, not 4, on purpose: goblin has commands that begin "GOBLIN."
// (GOBLIN.MEMORY, GOBLIN.CAD, ...), so an *inline* RESP command would start with the
// bytes "GOBLIN." -- a 4-byte "GOBL" magic would misfire on goblin's own commands.
// "GOBLINS!" diverges from "GOBLIN." at byte 6 ('S' vs '.'), so it cannot collide.
//
// See docs/sbe-protocol.md.

#include <algorithm>
#include <cstring>
#include <string_view>

namespace goblin::core {

// The magic as the 8 bytes it occupies on the wire, in order.
inline constexpr char kGoblinMagicBytes[8] = {'G', 'O', 'B', 'L', 'I', 'N', 'S', '!'};

// Whether a (possibly still-arriving) prefix is the SBE-wire magic.
enum class MagicMatch {
  yes,        // the full magic is present -> switch to SBE
  no,         // the bytes already diverge from the magic -> RESP
  need_more,  // matches so far but < 8 bytes -> wait for more before deciding
};

// Decide as soon as it is provable: the moment the prefix diverges we can answer
// `no` even with fewer than 8 bytes, so a short inline RESP command (e.g. "PING\r\n",
// 6 bytes) is never stalled waiting for a full 8-byte magic that will not come.
[[nodiscard]] inline MagicMatch match_goblin_magic(std::string_view bytes) noexcept {
  const std::size_t n = std::min(bytes.size(), sizeof(kGoblinMagicBytes));
  if (std::memcmp(bytes.data(), kGoblinMagicBytes, n) != 0) {
    return MagicMatch::no;
  }
  return bytes.size() >= sizeof(kGoblinMagicBytes) ? MagicMatch::yes : MagicMatch::need_more;
}

}  // namespace goblin::core
