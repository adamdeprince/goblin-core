#pragma once

// Server-side SBE dispatch: decode one length-prefixed SBE request straight out of
// the ring accumulator, apply it to the Store, and append a length-prefixed SBE
// reply. No SBE types appear in the signature (the generated codecs stay an
// implementation detail of the .cpp), so this header pulls in no SBE headers.

#include <cstddef>
#include <string>
#include <string_view>

namespace goblin::core {

class Store;
struct CommandExecutionOptions;

// Decode one complete SBE frame at the front of `bytes`, apply it to `store`, and
// append the length-prefixed SBE reply to `out`. Returns the number of bytes
// consumed, or 0 if a complete frame has not yet arrived (caller buffers and
// retries). A malformed/hostile frame is consumed without a reply, never a crash.
// `options` carries the script engines (for EVAL/EVALSHA/SCRIPT); the data commands
// ignore it.
[[nodiscard]] std::size_t sbe_dispatch_one(Store& store, std::string_view bytes, std::string& out,
                                           const CommandExecutionOptions& options);

}  // namespace goblin::core
