#pragma once

// Length-prefixed framing for the SBE wire. An SBE message is not self-delimiting
// -- its repeating groups and variable-length data make the total size dynamic -- so
// each SBE message on the stream is prefixed with its total byte length:
//
//     [ u32 little-endian total-message-bytes ][ SBE message (header + body) ]
//
// This is the moral equivalent of the FIX Simple Open Framing Header, kept minimal
// (no encoding-type field: the endpoint is already switched to SBE by the "GOBLINS!"
// magic, see goblin_protocol.hpp).

#include <cstddef>
#include <cstdint>

namespace goblin::core {

// Bytes of the little-endian u32 length that prefixes every SBE frame.
inline constexpr std::size_t kSbeLenPrefix = sizeof(std::uint32_t);

}  // namespace goblin::core
