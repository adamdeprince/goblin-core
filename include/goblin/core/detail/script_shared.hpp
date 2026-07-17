#pragma once

// Interpreter-agnostic helpers shared by the two scripting engines (PUC-Lua 5.1
// in src/script.cpp and Luau in src/luau_script.cpp). Nothing here touches any
// Lua C API, so it is safe to include alongside either runtime's headers.

#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "goblin/core/parse_int.hpp"
#include "goblin/core/resp_writer.hpp"

namespace goblin::core::script_shared {

// --- SHA1 (FIPS 180-1 / RFC 3174) ------------------------------------------
// Used for SCRIPT LOAD, EVALSHA lookup, and redis.sha1hex, on both engines.
class Sha1 {
 public:
  Sha1() { reset(); }

  void update(const unsigned char* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
      block_[block_len_++] = data[i];
      if (block_len_ == 64) {
        transform();
        length_bits_ += 512;
        block_len_ = 0;
      }
    }
  }

  // Writes 40 lowercase hex characters to `out`.
  void finalize_hex(char* out) {
    const std::uint64_t total_bits =
        length_bits_ + static_cast<std::uint64_t>(block_len_) * 8;
    block_[block_len_++] = 0x80;
    if (block_len_ > 56) {
      while (block_len_ < 64) block_[block_len_++] = 0;
      transform();
      block_len_ = 0;
    }
    while (block_len_ < 56) block_[block_len_++] = 0;
    for (int i = 7; i >= 0; --i) {
      block_[block_len_++] =
          static_cast<unsigned char>((total_bits >> (i * 8)) & 0xff);
    }
    transform();

    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 5; ++i) {
      for (int j = 7; j >= 0; --j) {
        *out++ = kHex[(state_[i] >> (j * 4)) & 0xf];
      }
    }
  }

 private:
  void reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    state_[4] = 0xC3D2E1F0;
    block_len_ = 0;
    length_bits_ = 0;
  }

  static std::uint32_t rol(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void transform() {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block_[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8) |
             (static_cast<std::uint32_t>(block_[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2];
    std::uint32_t d = state_[3], e = state_[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f;
      std::uint32_t k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  std::uint32_t state_[5];
  std::uint64_t length_bits_;
  unsigned char block_[64];
  std::size_t block_len_;
};

inline void sha1_hex_into(std::string_view data, char* out40) {
  Sha1 sha;
  sha.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  sha.finalize_hex(out40);
}

[[nodiscard]] inline std::string sha1_hex(std::string_view data) {
  std::string result(40, '\0');
  sha1_hex_into(data, result.data());
  return result;
}

// --- small string helpers ---------------------------------------------------

[[nodiscard]] inline char lower_char(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline std::string to_lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) c = lower_char(c);
  return result;
}

[[nodiscard]] inline bool equals_upper(std::string_view text,
                                       std::string_view upper) {
  if (text.size() != upper.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    if (u != upper[i]) return false;
  }
  return true;
}

// Commands a script may not invoke via redis.call: scripting/transaction entry
// points and commands that mutate connection state (HELLO/SUBSCRIBE).
[[nodiscard]] inline bool command_blocked_in_script(std::string_view name) {
  return equals_upper(name, "EVAL") || equals_upper(name, "EVALSHA") ||
         equals_upper(name, "SCRIPT") || equals_upper(name, "LUAU.EVAL") ||
         equals_upper(name, "LUAU.EVALSHA") || equals_upper(name, "LUAU.SCRIPT") ||
         equals_upper(name, "WREN.EVAL") || equals_upper(name, "WREN.EVALSHA") ||
         equals_upper(name, "WREN.SCRIPT") || equals_upper(name, "TCL.EVAL") ||
         equals_upper(name, "TCL.EVALSHA") || equals_upper(name, "TCL.SCRIPT") ||
         equals_upper(name, "UPYTHON.EVAL") || equals_upper(name, "UPYTHON.EVALSHA") ||
         equals_upper(name, "UPYTHON.SCRIPT") || equals_upper(name, "QUICKJS.EVAL") ||
         equals_upper(name, "QUICKJS.EVALSHA") || equals_upper(name, "QUICKJS.SCRIPT") ||
         equals_upper(name, "MULTI") ||
         equals_upper(name, "EXEC") || equals_upper(name, "WATCH") ||
         equals_upper(name, "SUBSCRIBE") || equals_upper(name, "UNSUBSCRIBE") ||
         equals_upper(name, "PSUBSCRIBE") || equals_upper(name, "PUNSUBSCRIBE") ||
         equals_upper(name, "PUBSUB") || equals_upper(name, "HELLO");
}

// Strip CRLF so a would-be RESP error/status line keeps the framing intact.
[[nodiscard]] inline std::string sanitize_line(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (char c : text) result.push_back((c == '\r' || c == '\n') ? ' ' : c);
  return result;
}

// A bare interpreter error ("user_script:3: ...") gets an "ERR " code; a message
// that already opens with an ALL-CAPS token (a propagated "WRONGTYPE ...") is
// left alone.
[[nodiscard]] inline std::string ensure_error_code(std::string_view message) {
  if (message.empty()) return "ERR script error";
  std::size_t token_end = 0;
  while (token_end < message.size() && message[token_end] != ' ') ++token_end;
  bool all_upper = token_end > 0;
  for (std::size_t i = 0; i < token_end; ++i) {
    if (message[i] < 'A' || message[i] > 'Z') {
      all_upper = false;
      break;
    }
  }
  if (all_upper) return std::string(message);
  return "ERR " + std::string(message);
}

[[nodiscard]] inline long long parse_signed(std::string_view text) {
  const auto value = goblin::core::parse_i64(text);
  return value ? *value : 0;
}

// Advance past one CRLF-terminated line, returning its content (sans CRLF) in
// `*line`. Used to walk a captured RESP reply.
[[nodiscard]] inline const char* read_line(const char* p, const char* end,
                                           std::string_view* line) {
  const char* start = p;
  while (p < end && *p != '\r') ++p;
  *line = std::string_view(start, static_cast<std::size_t>(p - start));
  if (p + 1 < end && p[0] == '\r' && p[1] == '\n') return p + 2;
  return end;  // malformed; stop
}

// Shared EVAL/EVALSHA argument shape: [body-or-sha, numkeys, key..., arg...].
// On success fills `keys`/`argv`; on failure writes a RESP error to `out` and
// returns false.
[[nodiscard]] inline bool split_keys_and_args(
    std::span<const std::string_view> args,
    std::span<const std::string_view>* keys,
    std::span<const std::string_view>* argv,
    std::string& out) {
  const auto parsed_numkeys = goblin::core::parse_i64(args[1]);
  if (!parsed_numkeys) {
    resp::append_error(out, "ERR value is not an integer or out of range");
    return false;
  }
  const long long numkeys = *parsed_numkeys;
  if (numkeys < 0) {
    resp::append_error(out, "ERR Number of keys can't be negative");
    return false;
  }
  if (static_cast<std::size_t>(numkeys) > args.size() - 2) {
    resp::append_error(out, "ERR Number of keys can't be greater than number of args");
    return false;
  }
  *keys = args.subspan(2, static_cast<std::size_t>(numkeys));
  *argv = args.subspan(2 + static_cast<std::size_t>(numkeys));
  return true;
}

}  // namespace goblin::core::script_shared
