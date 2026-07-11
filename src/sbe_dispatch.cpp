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
// Keyspace / TTL (batch 2)
#include "goblin_sbe/Del.h"
#include "goblin_sbe/Exists.h"
#include "goblin_sbe/Expire.h"
#include "goblin_sbe/ExpireAt.h"
#include "goblin_sbe/ExpireTime.h"
#include "goblin_sbe/PExpire.h"
#include "goblin_sbe/PExpireAt.h"
#include "goblin_sbe/PExpireTime.h"
#include "goblin_sbe/PTtl.h"
#include "goblin_sbe/Persist.h"
#include "goblin_sbe/Ttl.h"
#include "goblin_sbe/Type.h"
// Hash (batch 3)
#include "goblin_sbe/HDel.h"
#include "goblin_sbe/HExists.h"
#include "goblin_sbe/HGet.h"
#include "goblin_sbe/HGetAll.h"
#include "goblin_sbe/HIncrBy.h"
#include "goblin_sbe/HKeys.h"
#include "goblin_sbe/HLen.h"
#include "goblin_sbe/HMGet.h"
#include "goblin_sbe/HSet.h"
#include "goblin_sbe/HSetNx.h"
#include "goblin_sbe/HStrLen.h"
#include "goblin_sbe/HVals.h"
#include "goblin_sbe/NullableArrayReply.h"
// Standard-command tail (batch 4)
#include "goblin_sbe/Echo.h"
#include "goblin_sbe/GetRange.h"
#include "goblin_sbe/MGet.h"
#include "goblin_sbe/MSet.h"
#include "goblin_sbe/SetRange.h"
#include "goblin_sbe/ZRem.h"
#include "goblin_sbe/ZRemRangeByScore.h"
#include "goblin_sbe/ZRevRank.h"
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
#include <span>
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
constexpr std::uint16_t kDel = sbe::Del::sbeTemplateId();
constexpr std::uint16_t kExists = sbe::Exists::sbeTemplateId();
constexpr std::uint16_t kType = sbe::Type::sbeTemplateId();
constexpr std::uint16_t kExpire = sbe::Expire::sbeTemplateId();
constexpr std::uint16_t kPExpire = sbe::PExpire::sbeTemplateId();
constexpr std::uint16_t kExpireAt = sbe::ExpireAt::sbeTemplateId();
constexpr std::uint16_t kPExpireAt = sbe::PExpireAt::sbeTemplateId();
constexpr std::uint16_t kTtl = sbe::Ttl::sbeTemplateId();
constexpr std::uint16_t kPTtl = sbe::PTtl::sbeTemplateId();
constexpr std::uint16_t kPersist = sbe::Persist::sbeTemplateId();
constexpr std::uint16_t kExpireTime = sbe::ExpireTime::sbeTemplateId();
constexpr std::uint16_t kPExpireTime = sbe::PExpireTime::sbeTemplateId();
constexpr std::uint16_t kHSet = sbe::HSet::sbeTemplateId();
constexpr std::uint16_t kHSetNx = sbe::HSetNx::sbeTemplateId();
constexpr std::uint16_t kHGet = sbe::HGet::sbeTemplateId();
constexpr std::uint16_t kHMGet = sbe::HMGet::sbeTemplateId();
constexpr std::uint16_t kHDel = sbe::HDel::sbeTemplateId();
constexpr std::uint16_t kHGetAll = sbe::HGetAll::sbeTemplateId();
constexpr std::uint16_t kHKeys = sbe::HKeys::sbeTemplateId();
constexpr std::uint16_t kHVals = sbe::HVals::sbeTemplateId();
constexpr std::uint16_t kHLen = sbe::HLen::sbeTemplateId();
constexpr std::uint16_t kHExists = sbe::HExists::sbeTemplateId();
constexpr std::uint16_t kHStrLen = sbe::HStrLen::sbeTemplateId();
constexpr std::uint16_t kHIncrBy = sbe::HIncrBy::sbeTemplateId();
constexpr std::uint16_t kZRevRank = sbe::ZRevRank::sbeTemplateId();
constexpr std::uint16_t kZRem = sbe::ZRem::sbeTemplateId();
constexpr std::uint16_t kZRemRangeByScore = sbe::ZRemRangeByScore::sbeTemplateId();
constexpr std::uint16_t kGetRange = sbe::GetRange::sbeTemplateId();
constexpr std::uint16_t kSetRange = sbe::SetRange::sbeTemplateId();
constexpr std::uint16_t kMSet = sbe::MSet::sbeTemplateId();
constexpr std::uint16_t kMGet = sbe::MGet::sbeTemplateId();
constexpr std::uint16_t kEcho = sbe::Echo::sbeTemplateId();

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

void reply_status(std::string& out, std::string_view s) {
  reply<sbe::StatusReply>(out, [&](sbe::StatusReply& r) {
    r.putStatus(s.data(), static_cast<std::uint32_t>(s.size()));
  });
}

// EXPIRE / PEXPIRE / EXPIREAT / PEXPIREAT: native amount + flags. `relative` adds to
// now (EXPIRE/PEXPIRE); false is absolute (the *AT variants). `unit_ms` is 1000 for
// the second-granularity commands, 1 for the millisecond ones. Mirrors command.cpp.
void handle_expire(Store& store, bool relative, long long unit_ms, long long amount,
                   unsigned flags, std::string_view key, std::string& out) {
  if ((flags & ExpireFlag::kNx) &&
      (flags & (ExpireFlag::kXx | ExpireFlag::kGt | ExpireFlag::kLt))) {
    reply_error(out, "ERR", "NX and XX, GT or LT options at the same time are not compatible");
    return;
  }
  if ((flags & ExpireFlag::kGt) && (flags & ExpireFlag::kLt)) {
    reply_error(out, "ERR", "GT and LT options at the same time are not compatible");
    return;
  }
  const auto now = store.now_ms();
  const auto when = compute_when_ms(relative ? now : 0, amount, unit_ms);
  if (!when) {
    reply_error(out, "ERR", "invalid expire time");
    return;
  }
  reply_int(out, store.expire_at_ms(key, *when, now, flags) ? 1 : 0);
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

// HMGET / MGET -> an array whose elements may individually be nil (present=0). Opt is
// std::optional<std::string_view> (HMGET, zero-copy store views) or
// std::optional<std::string> (MGET, joined values) -- both expose ->data()/->size().
template <class Opt>
void reply_nullable_array(std::string& out, const std::vector<Opt>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  std::size_t body = 4;  // group header
  for (const auto& it : items) body += 1 + 4 + (it ? it->size() : 0);  // present + len + bytes
  std::vector<char>& rbuf = array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::NullableArrayReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& g = r.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto& it : items) {
    g.next();
    if (it) {
      g.present(1).putValue(it->data(), static_cast<std::uint32_t>(it->size()));
    } else {
      g.present(0).putValue("", 0);
    }
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

    case kDel: {
      sbe::Del d;
      d.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      long long removed = 0;
      auto& keys = d.keys();
      while (keys.hasNext()) {
        keys.next();
        removed += store.del(keys.getKeyAsStringView()) ? 1 : 0;
      }
      reply_int(out, removed);
      break;
    }

    case kExists: {
      sbe::Exists e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const bool has_ttl = !store.ttl_empty();
      const auto now = has_ttl ? store.now_ms() : std::uint64_t{0};
      long long count = 0;
      auto& keys = e.keys();
      while (keys.hasNext()) {
        keys.next();
        const std::string_view key = keys.getKeyAsStringView();
        if (has_ttl) {
          (void)store.purge_if_expired(key, now);
        }
        count += store.exists(key) ? 1 : 0;
      }
      reply_int(out, count);
      break;
    }

    case kType: {
      sbe::Type t;
      t.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto type = store.key_type(t.getKeyAsStringView());
      std::string_view name = "none";
      if (type) {
        switch (*type) {
          case KeyType::String: name = "string"; break;
          case KeyType::Zset: name = "zset"; break;
          case KeyType::Hash: name = "hash"; break;
        }
      }
      reply_status(out, name);
      break;
    }

    case kExpire: {
      sbe::Expire e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      handle_expire(store, true, 1000, amount, flags, e.getKeyAsStringView(), out);
      break;
    }
    case kPExpire: {
      sbe::PExpire e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      handle_expire(store, true, 1, amount, flags, e.getKeyAsStringView(), out);
      break;
    }
    case kExpireAt: {
      sbe::ExpireAt e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      handle_expire(store, false, 1000, amount, flags, e.getKeyAsStringView(), out);
      break;
    }
    case kPExpireAt: {
      sbe::PExpireAt e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      handle_expire(store, false, 1, amount, flags, e.getKeyAsStringView(), out);
      break;
    }

    case kTtl: {
      sbe::Ttl t;
      t.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto ms = store.pttl_ms(t.getKeyAsStringView(), store.now_ms());
      reply_int(out, ms < 0 ? ms : (ms + 500) / 1000);  // TTL rounds to seconds
      break;
    }
    case kPTtl: {
      sbe::PTtl t;
      t.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, store.pttl_ms(t.getKeyAsStringView(), store.now_ms()));
      break;
    }

    case kPersist: {
      sbe::Persist p;
      p.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, store.persist(p.getKeyAsStringView()) ? 1 : 0);
      break;
    }

    case kExpireTime: {
      sbe::ExpireTime e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto ms = store.expiretime_ms(e.getKeyAsStringView());
      reply_int(out, ms < 0 ? ms : ms / 1000);  // EXPIRETIME in seconds
      break;
    }
    case kPExpireTime: {
      sbe::PExpireTime e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, store.expiretime_ms(e.getKeyAsStringView()));
      break;
    }

    case kHSet: {
      sbe::HSet h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::pair<std::string_view, std::string_view>> entries;
      entries.clear();
      bool too_big = false;
      auto& g = h.entries();
      while (g.hasNext()) {
        g.next();
        const auto f = g.getFieldAsStringView();
        const auto v = g.getValueAsStringView();
        if (f.size() > Store::max_value_bytes() || v.size() > Store::max_value_bytes()) {
          too_big = true;
        }
        entries.emplace_back(f, v);
      }
      const std::string_view key = h.getKeyAsStringView();
      if (too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      long long added = 0;
      for (const auto& [f, v] : entries) added += store.hset(key, f, v);
      reply_int(out, added);
      break;
    }

    case kHSetNx: {
      sbe::HSetNx h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      const std::string_view value = h.getValueAsStringView();
      if (field.size() > Store::max_value_bytes() || value.size() > Store::max_value_bytes()) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_int(out, store.hsetnx(key, field, value));
      break;
    }

    case kHGet: {
      sbe::HGet h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      const auto v = store.hget(key, field);
      if (v) {
        reply_bulk(out, *v);
      } else {
        reply_nil(out);
      }
      break;
    }

    case kHMGet: {
      sbe::HMGet h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> fields;
      fields.clear();
      auto& g = h.fields();
      while (g.hasNext()) {
        g.next();
        fields.push_back(g.getFieldAsStringView());
      }
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::optional<std::string_view>> values;
      values.clear();
      for (const auto& f : fields) values.push_back(store.hget(key, f));
      reply_nullable_array(out, values);
      break;
    }

    case kHDel: {
      sbe::HDel h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> fields;
      fields.clear();
      auto& g = h.fields();
      while (g.hasNext()) {
        g.next();
        fields.push_back(g.getFieldAsStringView());
      }
      const std::string_view key = h.getKeyAsStringView();
      long long removed = 0;
      for (const auto& f : fields) removed += store.hdel(key, f) ? 1 : 0;
      reply_int(out, removed);
      break;
    }

    case kHGetAll: {
      sbe::HGetAll h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::string_view> flat;
      flat.clear();
      store.hash_for_each(key, [&](std::string_view f, std::string_view v) {
        flat.push_back(f);
        flat.push_back(v);
      });
      reply_array(out, flat);
      break;
    }

    case kHKeys: {
      sbe::HKeys h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::string_view> keys;
      keys.clear();
      store.hash_for_each(key, [&](std::string_view f, std::string_view) { keys.push_back(f); });
      reply_array(out, keys);
      break;
    }

    case kHVals: {
      sbe::HVals h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::string_view> vals;
      vals.clear();
      store.hash_for_each(key, [&](std::string_view, std::string_view v) { vals.push_back(v); });
      reply_array(out, vals);
      break;
    }

    case kHLen: {
      sbe::HLen h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, static_cast<long long>(store.hlen(h.getKeyAsStringView())));
      break;
    }

    case kHExists: {
      sbe::HExists h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      reply_int(out, store.hexists(key, field) ? 1 : 0);
      break;
    }

    case kHStrLen: {
      sbe::HStrLen h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      reply_int(out, static_cast<long long>(store.hstrlen(key, field).value_or(0)));
      break;
    }

    case kHIncrBy: {
      sbe::HIncrBy h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long delta = h.delta();
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      const auto result = store.hincrby(key, field, delta);
      if (result) {
        reply_int(out, *result);
      } else {
        reply_error(out, "ERR", "hash value is not an integer or out of range");
      }
      break;
    }

    case kZRevRank: {
      sbe::ZRevRank z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = z.getKeyAsStringView();
      const std::string_view member = z.getMemberAsStringView();
      const auto rank = store.zrevrank(key, member);
      if (rank) {
        reply_int(out, static_cast<long long>(*rank));
      } else {
        reply_nil(out);
      }
      break;
    }

    case kZRem: {
      sbe::ZRem z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> members;
      members.clear();
      auto& g = z.members();
      while (g.hasNext()) {
        g.next();
        members.push_back(g.getMemberAsStringView());
      }
      const std::string_view key = z.getKeyAsStringView();
      reply_int(out, store.zrem(key, std::span<const std::string_view>(members)));
      break;
    }

    case kZRemRangeByScore: {
      sbe::ZRemRangeByScore z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double min = z.min();
      const bool min_excl = z.minExclusive() != 0;
      const double max = z.max();
      const bool max_excl = z.maxExclusive() != 0;
      const std::string_view key = z.getKeyAsStringView();
      reply_int(out, store.zremrangebyscore(key, min, min_excl, max, max_excl));
      break;
    }

    case kGetRange: {
      sbe::GetRange g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long start = g.start();
      const long long end = g.end();
      reply_bulk(out, store.getrange(g.getKeyAsStringView(), start, end));
      break;
    }

    case kSetRange: {
      sbe::SetRange s;
      s.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long offset = s.byteOffset();
      const std::string_view key = s.getKeyAsStringView();
      const std::string_view value = s.getValueAsStringView();
      if (offset < 0) {
        reply_error(out, "ERR", "offset is out of range");
        break;
      }
      const auto len = store.setrange(key, static_cast<std::size_t>(offset), value);
      if (len) {
        reply_int(out, static_cast<long long>(*len));
      } else {
        reply_error(out, "ERR", kValueTooLargeMsg);
      }
      break;
    }

    case kMSet: {
      sbe::MSet m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::pair<std::string_view, std::string_view>> pairs;
      pairs.clear();
      bool too_big = false;
      auto& g = m.pairs();
      while (g.hasNext()) {
        g.next();
        const auto k = g.getKeyAsStringView();
        const auto v = g.getValueAsStringView();
        if (v.size() > Store::max_value_bytes()) too_big = true;
        pairs.emplace_back(k, v);
      }
      if (too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      for (const auto& [k, v] : pairs) store.set(k, v);
      reply_status(out, "OK");
      break;
    }

    case kMGet: {
      sbe::MGet m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> keys;
      keys.clear();
      auto& g = m.keys();
      while (g.hasNext()) {
        g.next();
        keys.push_back(g.getKeyAsStringView());
      }
      const bool has_ttl = !store.ttl_empty();
      const auto now = has_ttl ? store.now_ms() : std::uint64_t{0};
      static thread_local std::vector<std::optional<std::string>> values;
      values.clear();
      for (const auto& k : keys) {
        if (has_ttl) (void)store.purge_if_expired(k, now);
        const auto v = store.get(k);  // nil for a missing or non-string key
        if (v) {
          std::string s;
          s.reserve(v->size());
          s.append(v->head);
          s.append(v->tail);
          values.push_back(std::move(s));
        } else {
          values.push_back(std::nullopt);
        }
      }
      reply_nullable_array(out, values);
      break;
    }

    case kEcho: {
      sbe::Echo e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_bulk(out, e.getMessageAsStringView());
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
