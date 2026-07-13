#include "goblin/core/rdb.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/store.hpp"

// See rdb.hpp for the provenance/licensing rule (clean-room, v6-v11, BSD sources
// only). Byte layouts below are from public RDB format descriptions.

namespace goblin::core::rdb {
namespace {

constexpr int kMinVersion = 6;   // Redis 2.6 floor
constexpr int kMaxVersion = 11;  // Redis 7.2
constexpr std::uint64_t kMaxBlob = std::uint64_t{1} << 40;
constexpr std::size_t kMaxMemberBytes = 65535;

enum RdbType : std::uint8_t {
  kString = 0, kList = 1, kSet = 2, kZset = 3, kHash = 4, kZset2 = 5,
  kModule = 6, kModule2 = 7, kHashZipmap = 9, kListZiplist = 10,
  kSetIntset = 11, kZsetZiplist = 12, kHashZiplist = 13, kListQuicklist = 14,
  kStream1 = 15, kHashListpack = 16, kZsetListpack = 17, kListQuicklist2 = 18,
  kStream2 = 19, kSetListpack = 20, kStream3 = 21,
};

enum RdbOpcode : std::uint8_t {
  kOpSlotInfo = 244, kOpFunction2 = 245, kOpFunction = 246, kOpModuleAux = 247,
  kOpIdle = 248, kOpFreq = 249, kOpAux = 250, kOpResizeDb = 251,
  kOpExpireMs = 252, kOpExpire = 253, kOpSelectDb = 254, kOpEof = 255,
};

// Reflected CRC64 (Jones polynomial), matching Redis's checksum.
std::uint64_t crc64_byte(std::uint64_t crc, std::uint8_t byte) {
  static const auto table = [] {
    std::array<std::uint64_t, 256> t{};
    constexpr std::uint64_t poly = 0x95ac9329ac4bc9b5ULL;  // reflected Jones
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint64_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  return table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
}

// Streaming reader over the RDB file; accumulates the running CRC.
class Reader {
 public:
  explicit Reader(std::istream& in) : in_(in) {}

  std::uint8_t byte() {
    const int c = in_.get();
    if (c == std::char_traits<char>::eof()) {
      throw rdb_error("unexpected end of RDB");
    }
    const auto b = static_cast<std::uint8_t>(c);
    crc_ = crc64_byte(crc_, b);
    return b;
  }

  std::string bytes(std::uint64_t n) {
    if (n > kMaxBlob) {
      throw rdb_error("implausible length in RDB");
    }
    std::string s(n, '\0');
    in_.read(s.data(), static_cast<std::streamsize>(n));
    if (static_cast<std::uint64_t>(in_.gcount()) != n) {
      throw rdb_error("unexpected end of RDB");
    }
    for (const char ch : s) {
      crc_ = crc64_byte(crc_, static_cast<std::uint8_t>(ch));
    }
    return s;
  }

  std::string trailer(std::uint64_t n) {  // read without touching the CRC
    std::string s(n, '\0');
    in_.read(s.data(), static_cast<std::streamsize>(n));
    if (static_cast<std::uint64_t>(in_.gcount()) != n) {
      throw rdb_error("unexpected end of RDB");
    }
    return s;
  }

  [[nodiscard]] std::uint64_t crc() const noexcept { return crc_; }

 private:
  std::istream& in_;
  std::uint64_t crc_ = 0;
};

// (value, is_special_encoding).
std::pair<std::uint64_t, bool> load_len(Reader& r) {
  const auto b = r.byte();
  switch (b >> 6) {
    case 0:
      return {static_cast<std::uint64_t>(b & 0x3F), false};
    case 1: {
      const auto b2 = r.byte();
      return {(static_cast<std::uint64_t>(b & 0x3F) << 8) | b2, false};
    }
    case 2:
      if (b == 0x80) {
        std::uint64_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | r.byte();
        return {v, false};
      }
      if (b == 0x81) {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | r.byte();
        return {v, false};
      }
      throw rdb_error("bad RDB length encoding");
    default:
      return {static_cast<std::uint64_t>(b & 0x3F), true};
  }
}

std::uint64_t load_length(Reader& r) {
  const auto [v, special] = load_len(r);
  if (special) throw rdb_error("expected a plain length in RDB");
  return v;
}

std::string lzf_decompress(Reader& r) {
  const auto clen = load_length(r);
  const auto ulen = load_length(r);
  if (ulen > kMaxBlob) throw rdb_error("implausible LZF length");
  const auto in = r.bytes(clen);
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  std::string out;
  out.reserve(ulen);
  std::size_t ip = 0;
  while (ip < in.size()) {
    unsigned ctrl = p[ip++];
    if (ctrl < 32) {
      const std::size_t len = ctrl + 1;
      if (ip + len > in.size()) throw rdb_error("corrupt LZF literal");
      out.append(reinterpret_cast<const char*>(p + ip), len);
      ip += len;
    } else {
      std::size_t len = ctrl >> 5;
      if (len == 7) {
        if (ip >= in.size()) throw rdb_error("corrupt LZF");
        len += p[ip++];
      }
      if (ip >= in.size()) throw rdb_error("corrupt LZF");
      const std::size_t back = ((ctrl & 0x1F) << 8) + p[ip++] + 1;
      if (back > out.size()) throw rdb_error("corrupt LZF backref");
      const std::size_t ref = out.size() - back;
      len += 2;
      for (std::size_t k = 0; k < len; ++k) out.push_back(out[ref + k]);
    }
  }
  if (out.size() != ulen) throw rdb_error("LZF length mismatch");
  return out;
}

std::string load_string(Reader& r) {
  const auto [len, special] = load_len(r);
  if (!special) return r.bytes(len);
  switch (len) {
    case 0:
      return std::to_string(static_cast<std::int8_t>(r.byte()));
    case 1: {
      const auto lo = r.byte();
      const auto hi = r.byte();
      return std::to_string(
          static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8))));
    }
    case 2: {
      std::uint32_t v = 0;
      for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(r.byte()) << (8 * i);
      return std::to_string(static_cast<std::int32_t>(v));
    }
    case 3:
      return lzf_decompress(r);
    default:
      throw rdb_error("bad RDB string encoding");
  }
}

double load_binary_double(Reader& r) {
  std::uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(r.byte()) << (8 * i);
  return std::bit_cast<double>(bits);
}

double load_ascii_double(Reader& r) {
  const auto len = r.byte();
  if (len == 255) return -std::numeric_limits<double>::infinity();
  if (len == 254) return std::numeric_limits<double>::infinity();
  if (len == 253) return std::numeric_limits<double>::quiet_NaN();
  const auto text = r.bytes(len);
  return std::strtod(text.c_str(), nullptr);
}

// Cursor over an in-memory blob (ziplist / listpack).
class Cursor {
 public:
  Cursor(std::string_view data) : p_(data.data()), end_(data.data() + data.size()) {}
  [[nodiscard]] std::uint8_t u8() {
    if (p_ >= end_) throw rdb_error("truncated container blob");
    return static_cast<std::uint8_t>(*p_++);
  }
  [[nodiscard]] std::uint8_t peek() const {
    if (p_ >= end_) throw rdb_error("truncated container blob");
    return static_cast<std::uint8_t>(*p_);
  }
  [[nodiscard]] std::string_view bytes(std::size_t n) {
    if (static_cast<std::size_t>(end_ - p_) < n) throw rdb_error("truncated container blob");
    std::string_view s(p_, n);
    p_ += n;
    return s;
  }
  void skip(std::size_t n) {
    if (static_cast<std::size_t>(end_ - p_) < n) throw rdb_error("truncated container blob");
    p_ += n;
  }
  [[nodiscard]] const char* pos() const noexcept { return p_; }

 private:
  const char* p_;
  const char* end_;
};

// A single container element: either an integer or a byte string.
struct Element {
  bool is_int = false;
  std::int64_t ival = 0;
  std::string_view sval;
};

std::int64_t le_int(Cursor& c, int bytes, bool sign_extend) {
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(c.u8()) << (8 * i);
  if (sign_extend && bytes < 8) {
    const std::uint64_t sign = std::uint64_t{1} << (8 * bytes - 1);
    if (v & sign) v |= ~((std::uint64_t{1} << (8 * bytes)) - 1);
  }
  return static_cast<std::int64_t>(v);
}

Element read_ziplist_entry(Cursor& c) {
  const auto prevlen = c.u8();
  if (prevlen == 254) c.skip(4);
  const auto e = c.u8();
  if ((e >> 6) == 0) return {false, 0, c.bytes(e & 0x3F)};
  if ((e >> 6) == 1) {
    const auto lo = c.u8();
    return {false, 0, c.bytes((static_cast<std::size_t>(e & 0x3F) << 8) | lo)};
  }
  if ((e >> 6) == 2) {
    std::size_t len = 0;
    for (int i = 0; i < 4; ++i) len = (len << 8) | c.u8();  // big-endian
    return {false, 0, c.bytes(len)};
  }
  switch (e) {
    case 0xC0: return {true, le_int(c, 2, true), {}};
    case 0xD0: return {true, le_int(c, 4, true), {}};
    case 0xE0: return {true, le_int(c, 8, true), {}};
    case 0xF0: return {true, le_int(c, 3, true), {}};
    case 0xFE: return {true, static_cast<std::int8_t>(c.u8()), {}};
    default:
      if (e >= 0xF1 && e <= 0xFD) return {true, static_cast<std::int64_t>(e & 0x0F) - 1, {}};
      throw rdb_error("bad ziplist entry encoding");
  }
}

Element read_listpack_entry(Cursor& c) {
  const char* start = c.pos();
  const auto e = c.u8();
  Element out;
  if ((e & 0x80) == 0) {
    out = {true, static_cast<std::int64_t>(e & 0x7F), {}};
  } else if ((e & 0xC0) == 0x80) {
    out = {false, 0, c.bytes(e & 0x3F)};
  } else if ((e & 0xE0) == 0xC0) {
    const auto lo = c.u8();
    std::int64_t v = (static_cast<std::int64_t>(e & 0x1F) << 8) | lo;  // 13-bit
    if (v & 0x1000) v -= 0x2000;
    out = {true, v, {}};
  } else if ((e & 0xF0) == 0xE0) {
    const auto lo = c.u8();
    out = {false, 0, c.bytes((static_cast<std::size_t>(e & 0x0F) << 8) | lo)};
  } else if (e == 0xF1) {
    out = {true, le_int(c, 2, true), {}};
  } else if (e == 0xF2) {
    out = {true, le_int(c, 3, true), {}};
  } else if (e == 0xF3) {
    out = {true, le_int(c, 4, true), {}};
  } else if (e == 0xF4) {
    out = {true, le_int(c, 8, true), {}};
  } else if (e == 0xF0) {
    std::size_t len = 0;
    for (int i = 0; i < 4; ++i) len |= static_cast<std::size_t>(c.u8()) << (8 * i);  // LE
    out = {false, 0, c.bytes(len)};
  } else if (e == 0xFE) {
    out = {true, static_cast<std::int8_t>(c.u8()), {}};
  } else {
    throw rdb_error("bad listpack entry encoding");
  }
  const auto entry_len = static_cast<std::size_t>(c.pos() - start);
  const std::size_t backlen = entry_len < 128          ? 1
                              : entry_len < 16384       ? 2
                              : entry_len < 2097152     ? 3
                              : entry_len < 268435456   ? 4
                                                        : 5;
  c.skip(backlen);
  return out;
}

// Turn a container element into a member string or a score.
std::string element_member(const Element& e) {
  return e.is_int ? std::to_string(e.ival) : std::string(e.sval);
}
double element_score(const Element& e) {
  if (e.is_int) return static_cast<double>(e.ival);
  return std::strtod(std::string(e.sval).c_str(), nullptr);
}

template <class Add>
void parse_ziplist_zset(std::string_view blob, Add&& add) {
  Cursor c(blob);
  c.skip(10);  // zlbytes(4) + zltail(4) + zllen(2)
  while (c.peek() != 0xFF) {
    const auto member = read_ziplist_entry(c);
    const auto score = read_ziplist_entry(c);
    add(element_member(member), element_score(score));
  }
}

template <class Add>
void parse_listpack_zset(std::string_view blob, Add&& add) {
  Cursor c(blob);
  c.skip(6);  // total-bytes(4) + num-elements(2)
  while (c.peek() != 0xFF) {
    const auto member = read_listpack_entry(c);
    const auto score = read_listpack_entry(c);
    add(element_member(member), element_score(score));
  }
}

template <class Add>
std::size_t parse_ziplist_values(std::string_view blob, Add&& add) {
  Cursor c(blob);
  c.skip(10);  // zlbytes(4) + zltail(4) + zllen(2)
  std::size_t count = 0;
  while (c.peek() != 0xFF) {
    add(element_member(read_ziplist_entry(c)));
    ++count;
  }
  return count;
}

template <class Add>
std::size_t parse_listpack_values(std::string_view blob, Add&& add) {
  Cursor c(blob);
  c.skip(6);  // total-bytes(4) + num-elements(2)
  std::size_t count = 0;
  while (c.peek() != 0xFF) {
    add(element_member(read_listpack_entry(c)));
    ++count;
  }
  return count;
}

}  // namespace

std::uint64_t crc64(std::string_view data) noexcept {
  std::uint64_t crc = 0;
  for (const char c : data) crc = crc64_byte(crc, static_cast<std::uint8_t>(c));
  return crc;
}

SnapshotLoadStats import(Store& store, std::istream& in) {
  store.clear();
  Reader r(in);
  SnapshotLoadStats stats;

  auto add_member = [&](std::string_view key, std::string_view member, double score) {
    if (std::isnan(score)) {
      throw rdb_error("NaN score in key '" + std::string(key) + "'");
    }
    if (std::isinf(score)) {
      score = std::copysign(std::numeric_limits<double>::max(), score);
    }
    if (member.size() > kMaxMemberBytes) {
      throw rdb_error("member exceeds 64 KiB in key '" + std::string(key) + "'");
    }
    (void)store.zadd(key, score, member);
    ++stats.members;
  };

  try {
    if (r.bytes(5) != "REDIS") throw rdb_error("not a Redis RDB file");
    const auto version_text = r.bytes(4);
    int version = 0;
    for (const char ch : version_text) {
      if (ch < '0' || ch > '9') throw rdb_error("bad RDB version");
      version = version * 10 + (ch - '0');
    }
    if (version < kMinVersion) {
      throw rdb_error("RDB older than Redis 2.6 (v" + std::to_string(version) +
                      "); re-save under Redis >= 2.6");
    }
    if (version > kMaxVersion) {
      throw rdb_error("RDB newer than supported (v" + std::to_string(version) + ")");
    }

    for (;;) {
      const auto op = r.byte();
      if (op == kOpEof) break;
      switch (op) {
        case kOpSelectDb: (void)load_length(r); continue;      // merge all DBs
        case kOpResizeDb: (void)load_length(r); (void)load_length(r); continue;
        case kOpExpireMs: (void)r.bytes(8); continue;          // TTLs dropped
        case kOpExpire: (void)r.bytes(4); continue;
        case kOpAux: (void)load_string(r); (void)load_string(r); continue;
        case kOpIdle: (void)load_length(r); continue;
        case kOpFreq: (void)r.byte(); continue;
        case kOpFunction2: (void)load_string(r); continue;
        case kOpModuleAux: throw rdb_error("RDB contains module data; not supported");
        case kOpSlotInfo:
          (void)load_length(r); (void)load_length(r); (void)load_length(r); continue;
        default: break;
      }

      const auto type = op;
      const auto key = load_string(r);
      auto add = [&](std::string_view member, double score) {
        add_member(key, member, score);
      };
      std::vector<std::string> list_values;
      auto add_list_value = [&](std::string value) {
        if (list_values.size() ==
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
          throw rdb_error("list is too large in key '" + key + "'");
        }
        list_values.push_back(std::move(value));
      };
      auto install_list = [&] {
        if (list_values.empty()) {
          return;
        }
        std::vector<std::string_view> values;
        values.reserve(list_values.size());
        for (const auto& value : list_values) {
          values.push_back(value);
        }
        try {
          (void)store.rpush(key, values);
        } catch (const std::length_error&) {
          throw rdb_error("list value does not fit the configured encoding in "
                          "key '" + key + "'");
        }
        stats.members += values.size();
        ++stats.keys;
      };

      switch (type) {
        case kString: (void)load_string(r); break;
        case kList: {
          const auto count = load_length(r);
          if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw rdb_error("list is too large in key '" + key + "'");
          }
          list_values.reserve(static_cast<std::size_t>(count));
          for (std::uint64_t i = 0; i < count; ++i) {
            add_list_value(load_string(r));
          }
          install_list();
          break;
        }
        case kSet: {
          for (auto n = load_length(r); n > 0; --n) (void)load_string(r);
          break;
        }
        case kHash: {
          for (auto n = load_length(r); n > 0; --n) {
            (void)load_string(r);
            (void)load_string(r);
          }
          break;
        }
        case kZset: {
          const auto n = load_length(r);
          for (std::uint64_t i = 0; i < n; ++i) {
            const auto member = load_string(r);
            add(member, load_ascii_double(r));
          }
          ++stats.keys;
          break;
        }
        case kZset2: {
          const auto n = load_length(r);
          for (std::uint64_t i = 0; i < n; ++i) {
            const auto member = load_string(r);
            add(member, load_binary_double(r));
          }
          ++stats.keys;
          break;
        }
        case kZsetZiplist:
          parse_ziplist_zset(load_string(r), add);
          ++stats.keys;
          break;
        case kZsetListpack:
          parse_listpack_zset(load_string(r), add);
          ++stats.keys;
          break;
        case kListZiplist: {
          (void)parse_ziplist_values(load_string(r), add_list_value);
          install_list();
          break;
        }
        case kSetIntset:
        case kHashZiplist:
        case kHashListpack:
        case kSetListpack:
          (void)load_string(r);  // single container blob
          break;
        case kListQuicklist: {
          const auto nodes = load_length(r);
          for (std::uint64_t i = 0; i < nodes; ++i) {
            (void)parse_ziplist_values(load_string(r), add_list_value);
          }
          install_list();
          break;
        }
        case kListQuicklist2: {
          const auto nodes = load_length(r);
          for (std::uint64_t i = 0; i < nodes; ++i) {
            const auto container = load_length(r);
            auto blob = load_string(r);
            if (container == 1) {  // QUICKLIST_NODE_CONTAINER_PLAIN
              add_list_value(std::move(blob));
            } else if (container == 2) {  // QUICKLIST_NODE_CONTAINER_PACKED
              (void)parse_listpack_values(blob, add_list_value);
            } else {
              throw rdb_error("unsupported quicklist node container");
            }
          }
          install_list();
          break;
        }
        case kHashZipmap:
          throw rdb_error("zipmap hash (Redis < 2.6); re-save under Redis >= 2.6");
        case kModule:
        case kModule2:
          throw rdb_error("RDB contains a module type; not supported");
        case kStream1:
        case kStream2:
        case kStream3:
          throw rdb_error("RDB contains a stream; migrate streams with the import script");
        default:
          throw rdb_error("unsupported RDB type " + std::to_string(type));
      }
    }

    const auto computed = r.crc();
    const auto trailer = r.trailer(8);
    std::uint64_t stored = 0;
    for (int i = 0; i < 8; ++i) {
      stored |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(trailer[i])) << (8 * i);
    }
    if (stored != 0 && stored != computed) {
      throw rdb_error("RDB CRC64 mismatch");
    }
    return stats;
  } catch (...) {
    store.clear();
    throw;
  }
}

}  // namespace goblin::core::rdb
