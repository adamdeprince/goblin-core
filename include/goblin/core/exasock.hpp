#pragma once

// Optional Cisco ExaSock / Nexus SmartNIC sockets support.
//
// ExaSock is not vendored. Builds that want it pass -DGOBLIN_CORE_ENABLE_EXASOCK=ON
// and must have the system ExaSock SDK installed (headers under exasock/, and
// libexasock_ext). At runtime, processes are normally launched under the
// `exasock` wrapper so TCP sockets bound to ExaNIC interfaces bypass the kernel.
//
// Without GOBLIN_HAS_EXASOCK the rest of Goblin Core compiles unchanged.

#if defined(GOBLIN_HAS_EXASOCK)

#include <exasock/extensions.h>
#include <exasock/socket.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace goblin::core::exasock {

// True when this translation unit was compiled with ExaSock support.
[[nodiscard]] inline constexpr bool compiled_in() noexcept { return true; }

// True when the process is running under the exasock LD_PRELOAD wrapper.
[[nodiscard]] inline bool loaded() noexcept {
  return ::exasock_loaded() != 0;
}

[[nodiscard]] inline std::uint32_t version_code() noexcept {
  return loaded() ? ::exasock_version_code() : 0;
}

[[nodiscard]] inline std::string_view version_text() noexcept {
  if (!loaded()) {
    return {};
  }
  const char* text = ::exasock_version_text();
  return text != nullptr ? std::string_view(text) : std::string_view{};
}

// MSG_EXA_WARM is only meaningful under ExaSock >= 2.2.0. Callers should still
// pass a realistic payload length so the warm path matches the real send.
[[nodiscard]] inline bool supports_frame_warm() noexcept {
  return loaded() &&
         ::exasock_version_code() >= EXASOCK_VERSION(2, 2, 0);
}

// Look up the ExaNIC device/port for an accelerated TCP fd. Returns false when
// the socket is not accelerated or ExaSock is not loaded.
[[nodiscard]] inline bool tcp_device(int fd, std::string& device,
                                     int& port_num) {
  if (!loaded() || fd < 0) {
    return false;
  }
  char buf[64]{};
  int port = -1;
  if (::exasock_tcp_get_device(fd, buf, sizeof(buf), &port) != 0) {
    return false;
  }
  device.assign(buf);
  port_num = port;
  return true;
}

// Best-effort frame warm: walks the accelerated TX path then discards the
// message. No-op when ExaSock is absent or too old.
inline void warm_send(int fd, std::string_view payload) noexcept {
  if (fd < 0 || !supports_frame_warm() || payload.empty()) {
    return;
  }
  (void)::send(fd, payload.data(), payload.size(), MSG_EXA_WARM);
}

// Optional ATE id for connect. Negative means "ordinary accelerated TCP".
struct ConnectOptions {
  int ate_id{-1};
  bool require_loaded{false};
};

}  // namespace goblin::core::exasock

#else  // !GOBLIN_HAS_EXASOCK

#include <cstdint>
#include <string>
#include <string_view>

namespace goblin::core::exasock {

[[nodiscard]] inline constexpr bool compiled_in() noexcept { return false; }
[[nodiscard]] inline bool loaded() noexcept { return false; }
[[nodiscard]] inline std::uint32_t version_code() noexcept { return 0; }
[[nodiscard]] inline std::string_view version_text() noexcept { return {}; }
[[nodiscard]] inline bool supports_frame_warm() noexcept { return false; }
[[nodiscard]] inline bool tcp_device(int, std::string&, int&) { return false; }
inline void warm_send(int, std::string_view) noexcept {}

struct ConnectOptions {
  int ate_id{-1};
  bool require_loaded{false};
};

}  // namespace goblin::core::exasock

#endif  // GOBLIN_HAS_EXASOCK
