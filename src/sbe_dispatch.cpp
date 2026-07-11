#include "goblin/core/sbe_dispatch.hpp"

#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/store.hpp"
#include "goblin/core/string_value.hpp"

#include "goblin_sbe/Append.h"
#include "goblin_sbe/ArrayReply.h"
#include "goblin_sbe/BulkReply.h"
#include "goblin_sbe/Decr.h"
#include "goblin_sbe/DecrBy.h"
#include "goblin_sbe/DoubleReply.h"
#include "goblin_sbe/ErrorReply.h"
#include "goblin_sbe/Get.h"
#include "goblin_sbe/GetDel.h"
#include "goblin_sbe/GetSet.h"
#include "goblin_sbe/Incr.h"
#include "goblin_sbe/IncrBy.h"
#include "goblin_sbe/IncrByFloat.h"
#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/NilReply.h"
#include "goblin_sbe/Ping.h"
#include "goblin_sbe/ScoredArrayReply.h"
#include "goblin_sbe/Set.h"
#include "goblin_sbe/SetNx.h"
#include "goblin_sbe/StatusReply.h"
#include "goblin_sbe/StrLen.h"
#include "goblin_sbe/ZAdd.h"
#include "goblin_sbe/ZCard.h"
#include "goblin_sbe/ZRange.h"
#include "goblin_sbe/ZRank.h"
#include "goblin_sbe/ZScore.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core {
namespace {

namespace sbe = goblin_sbe;

// Request template ids as constants so they can be switch case labels (the switch
// over these dense ids compiles to a jump table -- the computed-goto dispatch).
constexpr std::uint16_t kPing = sbe::Ping::sbeTemplateId();
constexpr std::uint16_t kZAdd = sbe::ZAdd::sbeTemplateId();
constexpr std::uint16_t kZCard = sbe::ZCard::sbeTemplateId();
constexpr std::uint16_t kZScore = sbe::ZScore::sbeTemplateId();
constexpr std::uint16_t kZRank = sbe::ZRank::sbeTemplateId();
constexpr std::uint16_t kZRange = sbe::ZRange::sbeTemplateId();
constexpr std::uint16_t kGet = sbe::Get::sbeTemplateId();
constexpr std::uint16_t kSet = sbe::Set::sbeTemplateId();
constexpr std::uint16_t kGetSet = sbe::GetSet::sbeTemplateId();
constexpr std::uint16_t kSetNx = sbe::SetNx::sbeTemplateId();
constexpr std::uint16_t kGetDel = sbe::GetDel::sbeTemplateId();
constexpr std::uint16_t kStrLen = sbe::StrLen::sbeTemplateId();
constexpr std::uint16_t kAppend = sbe::Append::sbeTemplateId();
constexpr std::uint16_t kIncr = sbe::Incr::sbeTemplateId();
constexpr std::uint16_t kDecr = sbe::Decr::sbeTemplateId();
constexpr std::uint16_t kIncrBy = sbe::IncrBy::sbeTemplateId();
constexpr std::uint16_t kDecrBy = sbe::DecrBy::sbeTemplateId();
constexpr std::uint16_t kIncrByFloat = sbe::IncrByFloat::sbeTemplateId();

// SBE numInGroup is uint16, so one group holds at most 65535 elements.
constexpr std::size_t kMaxGroup = 65535;

// Error messages mirror src/command.cpp (Redis-spec text), split into code + message
// so a client can reconstruct the RESP "-<code> <message>" line.
constexpr std::string_view kErrNotInteger = "value is not an integer or out of range";
constexpr std::string_view kErrNotFloat = "value is not a valid float";
constexpr std::string_view kErrSyntax = "syntax error";
constexpr std::string_view kWrongTypeMsg = "Operation against a key holding the wrong kind of value";
constexpr std::string_view kValueTooLargeMsg =
    "value is larger than the 64 KiB limit; use https://goblin-store.dev";

// SET expire: absolute ms from base + amount*unit, clamped exactly like command.cpp's
// compute_when_ms (past -> 0, beyond the 48-bit TTL horizon -> nullopt).
std::optional<std::uint64_t> compute_when_ms(std::uint64_t base_ms, long long amount,
                                             long long unit_ms) {
  const __int128 when =
      static_cast<__int128>(base_ms) + static_cast<__int128>(amount) * unit_ms;
  if (when < 0) {
    return std::uint64_t{0};
  }
  if (when >= (static_cast<__int128>(1) << 48)) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(when);
}

// Offset of a message body: the 8-byte SBE header.
constexpr std::uint64_t kBodyOffset = sbe::MessageHeader::encodedLength();

// Build a reply message into a length-prefixed frame appended to `out`. Works for
// every scalar / var-data reply (Nil, Status, Error, Int, Double); array replies
// (a later pattern) will need a growable buffer instead of this fixed scratch.
template <class Msg, class Fill>
void reply(std::string& out, Fill&& fill) {
  alignas(8) char scratch[512];
  Msg msg;
  msg.wrapAndApplyHeader(scratch, kSbeLenPrefix, sizeof(scratch));
  fill(msg);
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + msg.encodedLength());
  std::memcpy(scratch, &total, kSbeLenPrefix);
  out.append(scratch, kSbeLenPrefix + total);
}

void reply_error(std::string& out, std::string_view code, std::string_view message) {
  reply<sbe::ErrorReply>(out, [&](sbe::ErrorReply& r) {
    r.putCode(code.data(), static_cast<std::uint32_t>(code.size()));
    r.putMessage(message.data(), static_cast<std::uint32_t>(message.size()));
  });
}

void reply_nil(std::string& out) { reply<sbe::NilReply>(out, [](sbe::NilReply&) {}); }

void reply_int(std::string& out, long long n) {
  reply<sbe::IntReply>(out, [n](sbe::IntReply& r) { r.value(n); });
}

void reply_int_or_range_error(std::string& out, std::optional<long long> v) {
  if (v) {
    reply_int(out, *v);
  } else {
    reply_error(out, "ERR", kErrNotInteger);
  }
}

// Array replies are variable-length, so unlike the fixed reply<> scratch they build
// into a heap-backed, reused buffer sized (an upper bound) per call. The exact frame
// length comes from the codec's encodedLength(), so over-sizing the buffer is safe.
// A group over 65535 elements cannot be expressed (SBE numInGroup is uint16) -> a
// clean error rather than a silent truncation (huge ranges must paginate).
std::vector<char>& array_scratch(std::size_t need) {
  static thread_local std::vector<char> buf;
  if (buf.size() < need) {
    buf.resize(need);
  }
  return buf;
}

// Bulk replies can be up to the 64 KiB value cap, so (like array replies) they build
// into the growable heap buffer, not the fixed reply<> scratch.
void reply_bulk(std::string& out, std::string_view v) {
  std::vector<char>& rbuf =
      array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + 4 + v.size() + 16);
  sbe::BulkReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  r.putValue(v.data(), static_cast<std::uint32_t>(v.size()));
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

void reply_bulk_or_nil(std::string& out, const std::optional<std::string>& v) {
  if (v) {
    reply_bulk(out, *v);
  } else {
    reply_nil(out);
  }
}

// GET returns a possibly head/tail-split value; join only on the rare split path.
void reply_bulk_value(std::string& out, const StringValueView& v) {
  if (v.tail.empty()) {
    reply_bulk(out, v.head);
  } else {
    static thread_local std::string joined;
    joined.clear();
    joined.reserve(v.size());
    joined.append(v.head);
    joined.append(v.tail);
    reply_bulk(out, joined);
  }
}

// ZRANGE -> array of member bulk strings.
void reply_array(std::string& out, const std::vector<std::string_view>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  std::size_t body = 4;  // group header (blockLength u16 + numInGroup u16)
  for (const auto& m : items) body += 4 + m.size();  // per entry: varData length + bytes
  std::vector<char>& rbuf = array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);

  sbe::ArrayReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& g = r.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto& m : items) {
    g.next().putValue(m.data(), static_cast<std::uint32_t>(m.size()));
  }
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

// ZRANGE WITHSCORES -> (member, native score) pairs.
void reply_scored_array(std::string& out,
                        const std::vector<std::pair<std::string_view, double>>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  std::size_t body = 4;  // group header
  for (const auto& [m, s] : items) body += 8 + 4 + m.size();  // score + varData length + bytes
  std::vector<char>& rbuf = array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);

  sbe::ScoredArrayReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& g = r.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto& [m, s] : items) {
    g.next().score(s).putMember(m.data(), static_cast<std::uint32_t>(m.size()));  // native double
  }
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

// Decode the request identified by `tid` straight out of `buf`, apply it to the
// store, and append the typed SBE reply. This is the per-command pattern every new
// command follows: wrapForDecode -> read native fields -> Store call -> reply<...>.
void handle(Store& store, std::uint16_t tid, char* buf, std::uint64_t buflen,
            std::uint16_t block_length, std::uint16_t version, std::string& out) {
  switch (tid) {
    case kPing:
      reply<sbe::StatusReply>(out, [](sbe::StatusReply& r) { r.putStatus("PONG", 4); });
      break;

    case kZAdd: {
      sbe::ZAdd z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      (void)z.flags();  // nx/xx/gt/lt/ch/incr -- plain ZADD for now
      // SBE lays the trailing key after the group, so buffer the (score, member)
      // pairs in one pass (member views stay valid), then read the key and apply.
      static thread_local std::vector<std::pair<double, std::string_view>> members;
      members.clear();
      auto& group = z.members();
      while (group.hasNext()) {
        group.next();
        members.emplace_back(group.score(), group.getMemberAsStringView());
      }
      const std::string_view key = z.getKeyAsStringView();
      long long added = 0;
      for (const auto& [score, member] : members) {
        added += store.zadd(key, score, member);  // native double, no re-parse
      }
      reply<sbe::IntReply>(out, [added](sbe::IntReply& r) { r.value(added); });
      break;
    }

    case kZCard: {
      sbe::ZCard z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long n = static_cast<long long>(store.zcard(z.getKeyAsStringView()));
      reply<sbe::IntReply>(out, [n](sbe::IntReply& r) { r.value(n); });
      break;
    }

    case kZScore: {
      sbe::ZScore z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = z.getKeyAsStringView();
      const std::string_view member = z.getMemberAsStringView();
      const auto score = store.zscore(key, member);
      if (score) {
        reply<sbe::DoubleReply>(out, [v = *score](sbe::DoubleReply& r) { r.value(v); });
      } else {
        reply<sbe::NilReply>(out, [](sbe::NilReply&) {});
      }
      break;
    }

    case kZRank: {
      sbe::ZRank z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = z.getKeyAsStringView();
      const std::string_view member = z.getMemberAsStringView();
      const auto rank = store.zrank(key, member);
      if (rank) {
        reply<sbe::IntReply>(out, [v = static_cast<long long>(*rank)](sbe::IntReply& r) { r.value(v); });
      } else {
        reply<sbe::NilReply>(out, [](sbe::NilReply&) {});
      }
      break;
    }

    case kZRange: {
      sbe::ZRange z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long start = z.start();
      const long long stop = z.stop();
      const bool with_scores = z.withScores() != 0;
      const bool rev = z.rev() != 0;
      const std::string_view key = z.getKeyAsStringView();

      // Native-double path (no score-string cache): the store streams count-first,
      // which is exactly what an SBE group needs. Collect one pass (views stay valid
      // for the read), then encode the array. WITHSCORES rides native doubles.
      if (with_scores) {
        static thread_local std::vector<std::pair<std::string_view, double>> items;
        items.clear();
        auto count_fn = [&](std::size_t n) { items.reserve(n); };
        auto value_fn = [&](std::string_view m, double s) { items.emplace_back(m, s); };
        if (rev) {
          store.zrevrange_values_for_each_counted(key, start, stop, count_fn, value_fn);
        } else {
          store.zrange_values_for_each_counted(key, start, stop, count_fn, value_fn);
        }
        reply_scored_array(out, items);
      } else {
        static thread_local std::vector<std::string_view> members;
        members.clear();
        auto count_fn = [&](std::size_t n) { members.reserve(n); };
        auto member_fn = [&](std::string_view m) { members.push_back(m); };
        if (rev) {
          store.zrevrange_members_for_each_counted(key, start, stop, count_fn, member_fn);
        } else {
          store.zrange_members_for_each_counted(key, start, stop, count_fn, member_fn);
        }
        reply_array(out, members);
      }
      break;
    }

    case kGet: {
      sbe::Get g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto v = store.get(g.getKeyAsStringView());
      if (v) {
        reply_bulk_value(out, *v);
      } else {
        reply_nil(out);
      }
      break;
    }

    case kSet: {
      sbe::Set s;
      s.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::uint8_t flags = s.flags();
      const std::uint8_t expire_mode = s.expireMode();
      const std::int64_t expire_value = s.expireValue();
      const std::string_view key = s.getKeyAsStringView();
      const std::string_view value = s.getValueAsStringView();

      const bool nx = (flags & 0x01u) != 0;
      const bool xx = (flags & 0x02u) != 0;
      const bool want_get = (flags & 0x04u) != 0;
      const bool keepttl = (flags & 0x08u) != 0;

      if (value.size() > Store::max_value_bytes()) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const auto now = store.now_ms();
      std::optional<std::uint64_t> expire_when;
      if (expire_mode != 0) {
        // 1 EX (s, relative), 2 PX (ms, relative), 3 EXAT (s, absolute), 4 PXAT (ms).
        const std::uint64_t base = (expire_mode == 1 || expire_mode == 2) ? now : 0;
        const long long unit = (expire_mode == 1 || expire_mode == 3) ? 1000 : 1;
        expire_when = compute_when_ms(base, expire_value, unit);
        if (!expire_when) {
          reply_error(out, "ERR", "invalid expire time in 'set' command");
          break;
        }
      }
      if ((nx && xx) || (keepttl && expire_when)) {
        reply_error(out, "ERR", kErrSyntax);
        break;
      }

      const bool exists = store.exists(key);
      if (want_get && exists && !store.key_is_string(key)) {
        reply_error(out, "WRONGTYPE", kWrongTypeMsg);
        break;
      }
      const bool condition_met = !(nx && exists) && !(xx && !exists);

      std::optional<std::string> old_value;  // captured before the overwrite
      if (want_get) {
        if (const auto current = store.get(key)) {
          old_value.emplace();
          old_value->reserve(current->size());
          old_value->append(current->head);
          old_value->append(current->tail);
        }
      }
      if (condition_met) {
        if (keepttl) {
          store.set_keep_ttl(key, value);
        } else {
          store.set(key, value);  // clears any existing TTL
        }
        if (expire_when) {
          (void)store.expire_at_ms(key, *expire_when, now);
        }
      }
      if (want_get) {
        if (old_value) {
          reply_bulk(out, *old_value);
        } else {
          reply_nil(out);
        }
      } else if (condition_met) {
        reply<sbe::StatusReply>(out, [](sbe::StatusReply& r) { r.putStatus("OK", 2); });
      } else {
        reply_nil(out);
      }
      break;
    }

    case kGetSet: {
      sbe::GetSet g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view value = g.getValueAsStringView();
      if (value.size() > Store::max_value_bytes()) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_bulk_or_nil(out, store.get_set(key, value));
      break;
    }

    case kSetNx: {
      sbe::SetNx g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view value = g.getValueAsStringView();
      if (value.size() > Store::max_value_bytes()) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_int(out, store.set_nx(key, value) ? 1 : 0);
      break;
    }

    case kGetDel: {
      sbe::GetDel g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_bulk_or_nil(out, store.get_del(g.getKeyAsStringView()));
      break;
    }

    case kStrLen: {
      sbe::StrLen g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto len = store.strlen(g.getKeyAsStringView());
      reply_int(out, len ? static_cast<long long>(*len) : 0);
      break;
    }

    case kAppend: {
      sbe::Append g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view value = g.getValueAsStringView();
      const auto current = store.strlen(key).value_or(0);
      if (current + value.size() > Store::max_value_bytes()) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_int(out, static_cast<long long>(store.append(key, value)));
      break;
    }

    case kIncr: {
      sbe::Incr g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int_or_range_error(out, store.incr_by(g.getKeyAsStringView(), 1));
      break;
    }

    case kDecr: {
      sbe::Decr g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int_or_range_error(out, store.incr_by(g.getKeyAsStringView(), -1));
      break;
    }

    case kIncrBy: {
      sbe::IncrBy g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::int64_t delta = g.delta();
      reply_int_or_range_error(out, store.incr_by(g.getKeyAsStringView(), delta));
      break;
    }

    case kDecrBy: {
      sbe::DecrBy g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::int64_t delta = g.delta();
      if (delta == std::numeric_limits<long long>::min()) {
        reply_error(out, "ERR", kErrNotInteger);  // negating LLONG_MIN overflows
        break;
      }
      reply_int_or_range_error(out, store.incr_by(g.getKeyAsStringView(), -delta));
      break;
    }

    case kIncrByFloat: {
      sbe::IncrByFloat g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double delta = g.delta();
      const auto result = store.incr_by_float(g.getKeyAsStringView(), delta);
      if (result) {
        reply_bulk(out, *result);  // formatted float, matches RESP
      } else {
        reply_error(out, "ERR", kErrNotFloat);
      }
      break;
    }

    default:
      reply_error(out, "ERR", "unknown command over the SBE wire");
      break;
  }
}

}  // namespace

std::size_t sbe_dispatch_one(Store& store, std::string_view bytes, std::string& out) {
  if (bytes.size() < kSbeLenPrefix) {
    return 0;  // not even a length yet
  }
  std::uint32_t msg_len = 0;
  std::memcpy(&msg_len, bytes.data(), kSbeLenPrefix);
  const std::size_t frame = kSbeLenPrefix + msg_len;
  if (bytes.size() < frame) {
    return 0;  // frame not fully arrived
  }

  // SBE flyweights take a mutable char* (they carry a read/write cursor); we only
  // read here, and the accumulator this views into is genuinely mutable, so the
  // const_cast is safe. Zero-copy: the request is decoded in place.
  char* buf = const_cast<char*>(bytes.data()) + kSbeLenPrefix;
  const std::uint64_t buflen = msg_len;

  try {
    sbe::MessageHeader hdr(buf, buflen);
    handle(store, hdr.templateId(), buf, buflen, hdr.blockLength(), hdr.version(), out);
  } catch (const std::exception&) {
    // Malformed / hostile frame (a bad length or var-data size trips SBE's bounds
    // check): consume it and resync, never crash.
  }
  return frame;
}

}  // namespace goblin::core
