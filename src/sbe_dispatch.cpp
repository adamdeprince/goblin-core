#include "goblin/core/sbe_dispatch.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/luau_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/script.hpp"
#include "goblin/core/store.hpp"
#include "goblin/core/string_value.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/wren_script.hpp"

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
#include "goblin_sbe/ScoredScanReply.h"
#include "goblin_sbe/StringScanReply.h"
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
#include "goblin_sbe/Scan.h"
// Hash (batch 3)
#include "goblin_sbe/HDel.h"
#include "goblin_sbe/HExists.h"
#include "goblin_sbe/HGet.h"
#include "goblin_sbe/HGetAll.h"
#include "goblin_sbe/HIncrBy.h"
#include "goblin_sbe/HScan.h"
#include "goblin_sbe/HKeys.h"
#include "goblin_sbe/HLen.h"
#include "goblin_sbe/HMGet.h"
#include "goblin_sbe/HSet.h"
#include "goblin_sbe/HSetNx.h"
#include "goblin_sbe/HStrLen.h"
#include "goblin_sbe/HVals.h"
#include "goblin_sbe/NullableArrayReply.h"
#include "goblin_sbe/NullableDoubleArrayReply.h"
// List (batch 9)
#include "goblin_sbe/LIndex.h"
#include "goblin_sbe/LInsert.h"
#include "goblin_sbe/LLen.h"
#include "goblin_sbe/LPop.h"
#include "goblin_sbe/LPush.h"
#include "goblin_sbe/LRange.h"
#include "goblin_sbe/LRem.h"
#include "goblin_sbe/LSet.h"
#include "goblin_sbe/LTrim.h"
#include "goblin_sbe/RPop.h"
#include "goblin_sbe/RPush.h"
// Sets (batch 11)
#include "goblin_sbe/SAdd.h"
#include "goblin_sbe/SCard.h"
#include "goblin_sbe/SDiff.h"
#include "goblin_sbe/SDiffStore.h"
#include "goblin_sbe/SInter.h"
#include "goblin_sbe/SInterCard.h"
#include "goblin_sbe/SInterStore.h"
#include "goblin_sbe/SIsMember.h"
#include "goblin_sbe/SMIsMember.h"
#include "goblin_sbe/SMembers.h"
#include "goblin_sbe/SMove.h"
#include "goblin_sbe/SPop.h"
#include "goblin_sbe/SRandMember.h"
#include "goblin_sbe/SRem.h"
#include "goblin_sbe/SScan.h"
#include "goblin_sbe/SUnion.h"
#include "goblin_sbe/SUnionStore.h"
// Standard-command tail (batch 4)
#include "goblin_sbe/Echo.h"
#include "goblin_sbe/GetRange.h"
#include "goblin_sbe/MGet.h"
#include "goblin_sbe/MSet.h"
#include "goblin_sbe/SetRange.h"
#include "goblin_sbe/ZRem.h"
#include "goblin_sbe/ZRemRangeByScore.h"
#include "goblin_sbe/ZRevRank.h"
// GOBLIN.* natives (batch 5)
#include "goblin_sbe/GoblinCad.h"
#include "goblin_sbe/GoblinCaExpire.h"
#include "goblin_sbe/GoblinCas.h"
#include "goblin_sbe/GoblinClaim.h"
#include "goblin_sbe/GoblinDecrPos.h"
#include "goblin_sbe/GoblinHCad.h"
#include "goblin_sbe/GoblinHSetGt.h"
#include "goblin_sbe/GoblinIncrBound.h"
#include "goblin_sbe/GoblinIncrEx.h"
#include "goblin_sbe/GoblinTdRescore.h"
#include "goblin_sbe/GoblinZWindow.h"
// Admin (batch 6)
#include "goblin_sbe/GoblinOptimize.h"
#include "goblin_sbe/Info.h"
// Scripting (batch 7)
#include "goblin_sbe/Eval.h"
#include "goblin_sbe/EvalSha.h"
#include "goblin_sbe/RespValueReply.h"
#include "goblin_sbe/Script.h"
// Admin persistence (batch 8)
#include "goblin_sbe/GoblinLoad.h"
#include "goblin_sbe/GoblinMemory.h"
#include "goblin_sbe/GoblinSave.h"
#include "goblin_sbe/MapReply.h"
#include "goblin_sbe/ZAdd.h"
#include "goblin_sbe/ZCard.h"
#include "goblin_sbe/ZCount.h"
#include "goblin_sbe/ZIncrBy.h"
#include "goblin_sbe/ZMScore.h"
#include "goblin_sbe/ZPop.h"
#include "goblin_sbe/ZRange.h"
#include "goblin_sbe/ZRangeByScore.h"
#include "goblin_sbe/ZRank.h"
#include "goblin_sbe/ZScan.h"
#include "goblin_sbe/ZScore.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
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
constexpr std::uint16_t kScan = sbe::Scan::sbeTemplateId();
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
constexpr std::uint16_t kHScan = sbe::HScan::sbeTemplateId();
constexpr std::uint16_t kZRevRank = sbe::ZRevRank::sbeTemplateId();
constexpr std::uint16_t kZRem = sbe::ZRem::sbeTemplateId();
constexpr std::uint16_t kZRemRangeByScore = sbe::ZRemRangeByScore::sbeTemplateId();
constexpr std::uint16_t kGetRange = sbe::GetRange::sbeTemplateId();
constexpr std::uint16_t kSetRange = sbe::SetRange::sbeTemplateId();
constexpr std::uint16_t kMSet = sbe::MSet::sbeTemplateId();
constexpr std::uint16_t kMGet = sbe::MGet::sbeTemplateId();
constexpr std::uint16_t kEcho = sbe::Echo::sbeTemplateId();
constexpr std::uint16_t kGoblinCad = sbe::GoblinCad::sbeTemplateId();
constexpr std::uint16_t kGoblinCaExpire = sbe::GoblinCaExpire::sbeTemplateId();
constexpr std::uint16_t kGoblinCas = sbe::GoblinCas::sbeTemplateId();
constexpr std::uint16_t kGoblinTdRescore = sbe::GoblinTdRescore::sbeTemplateId();
constexpr std::uint16_t kGoblinIncrEx = sbe::GoblinIncrEx::sbeTemplateId();
constexpr std::uint16_t kGoblinZWindow = sbe::GoblinZWindow::sbeTemplateId();
constexpr std::uint16_t kGoblinIncrBound = sbe::GoblinIncrBound::sbeTemplateId();
constexpr std::uint16_t kGoblinDecrPos = sbe::GoblinDecrPos::sbeTemplateId();
constexpr std::uint16_t kGoblinHCad = sbe::GoblinHCad::sbeTemplateId();
constexpr std::uint16_t kGoblinHSetGt = sbe::GoblinHSetGt::sbeTemplateId();
constexpr std::uint16_t kGoblinClaim = sbe::GoblinClaim::sbeTemplateId();
constexpr std::uint16_t kGoblinOptimize = sbe::GoblinOptimize::sbeTemplateId();
constexpr std::uint16_t kInfo = sbe::Info::sbeTemplateId();
constexpr std::uint16_t kEval = sbe::Eval::sbeTemplateId();
constexpr std::uint16_t kEvalSha = sbe::EvalSha::sbeTemplateId();
constexpr std::uint16_t kScript = sbe::Script::sbeTemplateId();
constexpr std::uint16_t kGoblinMemory = sbe::GoblinMemory::sbeTemplateId();
constexpr std::uint16_t kGoblinSave = sbe::GoblinSave::sbeTemplateId();
constexpr std::uint16_t kGoblinLoad = sbe::GoblinLoad::sbeTemplateId();
constexpr std::uint16_t kLPush = sbe::LPush::sbeTemplateId();
constexpr std::uint16_t kRPush = sbe::RPush::sbeTemplateId();
constexpr std::uint16_t kLPop = sbe::LPop::sbeTemplateId();
constexpr std::uint16_t kRPop = sbe::RPop::sbeTemplateId();
constexpr std::uint16_t kLLen = sbe::LLen::sbeTemplateId();
constexpr std::uint16_t kLIndex = sbe::LIndex::sbeTemplateId();
constexpr std::uint16_t kLRange = sbe::LRange::sbeTemplateId();
constexpr std::uint16_t kLSet = sbe::LSet::sbeTemplateId();
constexpr std::uint16_t kLTrim = sbe::LTrim::sbeTemplateId();
constexpr std::uint16_t kLRem = sbe::LRem::sbeTemplateId();
constexpr std::uint16_t kLInsert = sbe::LInsert::sbeTemplateId();
constexpr std::uint16_t kSAdd = sbe::SAdd::sbeTemplateId();
constexpr std::uint16_t kSRem = sbe::SRem::sbeTemplateId();
constexpr std::uint16_t kSCard = sbe::SCard::sbeTemplateId();
constexpr std::uint16_t kSIsMember = sbe::SIsMember::sbeTemplateId();
constexpr std::uint16_t kSMIsMember = sbe::SMIsMember::sbeTemplateId();
constexpr std::uint16_t kSMembers = sbe::SMembers::sbeTemplateId();
constexpr std::uint16_t kSPop = sbe::SPop::sbeTemplateId();
constexpr std::uint16_t kSRandMember = sbe::SRandMember::sbeTemplateId();
constexpr std::uint16_t kSMove = sbe::SMove::sbeTemplateId();
constexpr std::uint16_t kSInter = sbe::SInter::sbeTemplateId();
constexpr std::uint16_t kSUnion = sbe::SUnion::sbeTemplateId();
constexpr std::uint16_t kSDiff = sbe::SDiff::sbeTemplateId();
constexpr std::uint16_t kSInterStore = sbe::SInterStore::sbeTemplateId();
constexpr std::uint16_t kSUnionStore = sbe::SUnionStore::sbeTemplateId();
constexpr std::uint16_t kSDiffStore = sbe::SDiffStore::sbeTemplateId();
constexpr std::uint16_t kSInterCard = sbe::SInterCard::sbeTemplateId();
constexpr std::uint16_t kSScan = sbe::SScan::sbeTemplateId();
constexpr std::uint16_t kZIncrBy = sbe::ZIncrBy::sbeTemplateId();
constexpr std::uint16_t kZRangeByScore = sbe::ZRangeByScore::sbeTemplateId();
constexpr std::uint16_t kZCount = sbe::ZCount::sbeTemplateId();
constexpr std::uint16_t kZMScore = sbe::ZMScore::sbeTemplateId();
constexpr std::uint16_t kZPop = sbe::ZPop::sbeTemplateId();
constexpr std::uint16_t kZScan = sbe::ZScan::sbeTemplateId();

// SBE numInGroup is uint16, so one group holds at most 65535 elements.
constexpr std::size_t kMaxGroup = 65535;

// Error messages mirror src/command.cpp (Redis-spec text), split into code + message
// so a client can reconstruct the RESP "-<code> <message>" line.
constexpr std::string_view kErrNotInteger = "value is not an integer or out of range";
constexpr std::string_view kErrNotFloat = "value is not a valid float";
constexpr std::string_view kErrSyntax = "syntax error";
constexpr std::string_view kWrongTypeMsg = "Operation against a key holding the wrong kind of value";
constexpr std::string_view kValueTooLargeMsg =
    "value does not fit the 65,535-byte encoded limit; use "
    "https://goblin-store.dev";

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

void reply_double(std::string& out, double value) {
  reply<sbe::DoubleReply>(out,
                          [value](sbe::DoubleReply& r) { r.value(value); });
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

// Bulk replies can carry the full 24-bit logical LZ4 value, so (like array
// replies) they build into the growable heap buffer, not the fixed reply<>
// scratch.
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

// SBE varData requires one contiguous logical payload. Prefer zero-copy into
// putValue when the stored form is already a contiguous raw/verbatim slice;
// otherwise decode once into a thread-local buffer (no second join+copy).
void reply_bulk_value(std::string& out, const StringValueView& v) {
  if (v.is_raw()) {
    if (!v.encoding_enabled()) {
      if (v.encoded_tail().empty()) {
        reply_bulk(out, v.encoded_head());
        return;
      }
    } else if (v.encoded_head().size() >= 1 && v.encoded_tail().empty()) {
      reply_bulk(out, std::string_view(v.encoded_head().data() + 1,
                                       v.encoded_head().size() - 1));
      return;
    }
  }
  static thread_local std::string joined;
  joined.clear();
  joined.reserve(v.size());
  v.append_to(joined);
  reply_bulk(out, joined);
}

// ZRANGE -> array of member bulk strings.
template <class Item>
void reply_array(std::string& out, const std::vector<Item>& items) {
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

void reply_nullable_double_array(
    std::string& out, const std::vector<std::optional<double>>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  const std::size_t body = 4 + items.size() * (1 + sizeof(double));
  std::vector<char>& rbuf = array_scratch(
      kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::NullableDoubleArrayReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& group = r.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto item : items) {
    group.next().present(item.has_value() ? 1 : 0).value(item.value_or(0.0));
  }
  const std::uint32_t total = static_cast<std::uint32_t>(
      sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

void reply_scored_scan(
    std::string& out, std::uint64_t next_cursor,
    const std::vector<std::pair<std::string_view, double>>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  std::size_t body = sizeof(std::uint64_t) + 4;
  for (const auto& [member, score] : items) {
    (void)score;
    body += sizeof(double) + 4 + member.size();
  }
  std::vector<char>& rbuf = array_scratch(
      kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::ScoredScanReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  r.nextCursor(next_cursor);
  auto& group = r.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto& [member, score] : items) {
    group.next().score(score).putMember(
        member.data(), static_cast<std::uint32_t>(member.size()));
  }
  const std::uint32_t total = static_cast<std::uint32_t>(
      sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

template <class String>
void reply_string_scan(std::string& out, std::uint64_t next_cursor,
                       const std::vector<String>& items) {
  if (items.size() > kMaxGroup) {
    reply_error(out, "ERR", "result too large for one SBE array reply");
    return;
  }
  std::size_t body = sizeof(std::uint64_t) + 4;
  for (const auto& item : items) {
    body += 4 + item.size();
  }
  std::vector<char>& rbuf = array_scratch(
      kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::StringScanReply reply;
  reply.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  reply.nextCursor(next_cursor);
  auto& group = reply.itemsCount(static_cast<std::uint16_t>(items.size()));
  for (const auto& item : items) {
    group.next().putValue(item.data(),
                          static_cast<std::uint32_t>(item.size()));
  }
  const std::uint32_t total = static_cast<std::uint32_t>(
      sbe::MessageHeader::encodedLength() + reply.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

// GOBLIN.MEMORY -> key/value pairs, from a flat [name, value, ...] field list.
void reply_map(std::string& out, const std::vector<std::string>& kv) {
  const std::size_t pairs = kv.size() / 2;
  if (pairs > kMaxGroup) {
    reply_error(out, "ERR", "map reply too large for the SBE wire");
    return;
  }
  std::size_t body = 4;  // group header
  for (const auto& s : kv) body += 4 + s.size();  // each string is one varData
  std::vector<char>& rbuf =
      array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::MapReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& g = r.entriesCount(static_cast<std::uint16_t>(pairs));
  for (std::size_t i = 0; i + 1 < kv.size(); i += 2) {
    g.next().putKey(kv[i].data(), static_cast<std::uint32_t>(kv[i].size()))
        .putValue(kv[i + 1].data(), static_cast<std::uint32_t>(kv[i + 1].size()));
  }
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

// --- Scripting (EVAL / EVALSHA / SCRIPT) ---------------------------------------

enum class ScriptOp { eval, eval_sha, script };

// Run a script verb on the engine selected by `lang`, writing its RESP reply to
// `resp`. Returns false if that language's engine is not configured. The generic
// lambda works because every engine exposes the same eval/eval_sha/script(args, out).
bool run_script(const CommandExecutionOptions& o, std::uint8_t lang, ScriptOp op,
                std::span<const std::string_view> args, std::string& resp) {
  const auto call = [&](auto* engine) -> bool {
    if (engine == nullptr) return false;
    switch (op) {
      case ScriptOp::eval: engine->eval(args, resp); break;
      case ScriptOp::eval_sha: engine->eval_sha(args, resp); break;
      case ScriptOp::script: engine->script(args, resp); break;
    }
    return true;
  };
  switch (lang) {
    case 0: return call(o.script_engine);   // Lua
    case 1: return call(o.luau_engine);
    case 2: return call(o.wren_engine);
    case 3: return call(o.tcl_engine);
    case 4: return call(o.upython_engine);
    case 5: return call(o.quickjs_engine);
    default: return false;
  }
}

// A flattened RESP node (pre-order); `bytes` views into the engine's RESP buffer.
struct RespNode {
  std::uint8_t type;  // 0 nil 1 int 2 bulk 3 status 4 error 5 array 6 map
  std::int64_t int_value;
  std::uint32_t child_count;
  std::string_view bytes;
};

[[nodiscard]] long long parse_ll_line(std::string_view line) {
  long long v = 0;
  std::from_chars(line.data(), line.data() + line.size(), v);
  return v;
}

// Parse one RESP value at d[pos] into pre-order nodes; return the position after it.
std::size_t parse_resp(std::string_view d, std::size_t pos, std::vector<RespNode>& nodes) {
  if (pos >= d.size()) return d.size();
  const char t = d[pos++];
  const std::size_t eol = d.find("\r\n", pos);
  if (eol == std::string_view::npos) return d.size();
  const std::string_view line = d.substr(pos, eol - pos);
  const std::size_t after = eol + 2;
  switch (t) {
    case ':': nodes.push_back({1, parse_ll_line(line), 0, {}}); return after;
    case '+': nodes.push_back({3, 0, 0, line}); return after;
    case '-': nodes.push_back({4, 0, 0, line}); return after;
    case '_': nodes.push_back({0, 0, 0, {}}); return after;  // RESP3 null
    case '$': {
      const long long len = parse_ll_line(line);
      if (len < 0) { nodes.push_back({0, 0, 0, {}}); return after; }  // $-1 null
      nodes.push_back({2, 0, 0, d.substr(after, static_cast<std::size_t>(len))});
      return after + static_cast<std::size_t>(len) + 2;
    }
    case '*':
    case '~': {  // array / set
      const long long n = parse_ll_line(line);
      if (n < 0) { nodes.push_back({0, 0, 0, {}}); return after; }  // *-1 null
      nodes.push_back({5, 0, static_cast<std::uint32_t>(n), {}});
      std::size_t p = after;
      for (long long i = 0; i < n; ++i) p = parse_resp(d, p, nodes);
      return p;
    }
    case '%': {  // map: n pairs -> 2n child nodes
      const long long n = parse_ll_line(line);
      nodes.push_back({6, 0, static_cast<std::uint32_t>(n < 0 ? 0 : n), {}});
      std::size_t p = after;
      for (long long i = 0; i < n * 2; ++i) p = parse_resp(d, p, nodes);
      return p;
    }
    default:  // #bool  ,double  (bignum ... -> pass the raw line through as a bulk
      nodes.push_back({2, 0, 0, line});
      return after;
  }
}

void reply_resp_value(std::string& out, const std::vector<RespNode>& nodes) {
  if (nodes.size() > kMaxGroup) {
    reply_error(out, "ERR", "script reply too large for the SBE wire");
    return;
  }
  std::size_t body = 4;  // group header
  for (const auto& n : nodes) body += 13 + 4 + n.bytes.size();  // type1+int8+cc4 + varData
  std::vector<char>& rbuf =
      array_scratch(kSbeLenPrefix + sbe::MessageHeader::encodedLength() + body + 16);
  sbe::RespValueReply r;
  r.wrapAndApplyHeader(rbuf.data(), kSbeLenPrefix, rbuf.size());
  auto& g = r.nodesCount(static_cast<std::uint16_t>(nodes.size()));
  for (const auto& n : nodes) {
    g.next().type(n.type).intValue(n.int_value).childCount(n.child_count)
        .putBytes(n.bytes.data(), static_cast<std::uint32_t>(n.bytes.size()));
  }
  const std::uint32_t total =
      static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + r.encodedLength());
  std::memcpy(rbuf.data(), &total, kSbeLenPrefix);
  out.append(rbuf.data(), kSbeLenPrefix + total);
}

// Run `eargs` on the engine, flatten its RESP reply into a RespValueReply.
void run_and_reply(const CommandExecutionOptions& o, std::uint8_t lang, ScriptOp op,
                   std::span<const std::string_view> eargs, std::string& out) {
  static thread_local std::string resp;
  resp.clear();
  if (!run_script(o, lang, op, eargs, resp)) {
    reply_error(out, "ERR", "This Redis command is not available");
    return;
  }
  static thread_local std::vector<RespNode> nodes;
  nodes.clear();
  parse_resp(resp, 0, nodes);
  reply_resp_value(out, nodes);
}

// EVAL / EVALSHA share this: reconstruct the engine's [code, numkeys, keys..., args...]
// argument vector (the engines re-parse numkeys) and run it.
void eval_dispatch(const CommandExecutionOptions& o, std::uint8_t lang, ScriptOp op,
                   std::string_view code, const std::vector<std::string_view>& keys,
                   const std::vector<std::string_view>& argv, std::string& out) {
  static thread_local std::string numkeys;
  numkeys = std::to_string(keys.size());
  static thread_local std::vector<std::string_view> eargs;
  eargs.clear();
  eargs.push_back(code);
  eargs.push_back(numkeys);
  eargs.insert(eargs.end(), keys.begin(), keys.end());
  eargs.insert(eargs.end(), argv.begin(), argv.end());
  run_and_reply(o, lang, op, eargs, out);
}

// Decode the request identified by `tid` straight out of `buf`, apply it to the
// store, and append the typed SBE reply. This is the per-command pattern every new
// command follows: wrapForDecode -> read native fields -> Store call -> reply<...>.
// ---- WRONGTYPE guard -------------------------------------------------------
// A key holds at most one type (one unified namespace), so a command that operates on
// a specific type must be rejected when the key already holds a different one -- this
// is command.cpp's central command_requires_type() check, mirrored for the SBE path.
// It is not cosmetic: the store's typed accessors assume the caller already checked
// (get_or_create_zset only *asserts* the type, and the assert is compiled out in a
// release build), so a wrong-type command would reinterpret the wrong union member and
// corrupt memory. SET/SETNX/MSET (clobber or create), MGET/DEL/EXISTS/TYPE/TTL
// (type-agnostic), scripts, and the introspection/admin GOBLIN.* commands are exempt.
[[nodiscard]] std::optional<KeyType> sbe_requires_type(std::uint16_t tid) {
  switch (tid) {
    case kZAdd: case kZCard: case kZScore: case kZRank: case kZRevRank:
    case kZRem: case kZRemRangeByScore: case kZRange: case kZIncrBy:
    case kZRangeByScore: case kZCount: case kZMScore: case kZPop: case kZScan:
    case kGoblinTdRescore: case kGoblinZWindow:
      return KeyType::Zset;
    case kHSet: case kHSetNx: case kHGet: case kHMGet: case kHDel:
    case kHGetAll: case kHKeys: case kHVals: case kHLen: case kHExists:
    case kHStrLen: case kHIncrBy: case kHScan:
    case kGoblinHCad: case kGoblinHSetGt:
      return KeyType::Hash;
    case kLPush: case kRPush: case kLPop: case kRPop: case kLLen:
    case kLIndex: case kLRange: case kLSet: case kLTrim: case kLRem:
    case kLInsert:
      return KeyType::List;
    case kSAdd: case kSRem: case kSCard: case kSIsMember: case kSMIsMember:
    case kSMembers: case kSPop: case kSRandMember: case kSMove: case kSScan:
      // Multi-key algebra/*STORE/SINTERCARD check sources in the handler.
      return KeyType::Set;
    // kGet is omitted: its handler fuses Missing/WrongType/Ok in one probe.
    case kGetSet: case kGetDel: case kStrLen: case kAppend:
    case kIncr: case kDecr: case kIncrBy: case kDecrBy: case kIncrByFloat:
    case kGetRange: case kSetRange:
    case kGoblinCad: case kGoblinCaExpire: case kGoblinCas:
    case kGoblinIncrEx: case kGoblinIncrBound: case kGoblinDecrPos:
      return KeyType::String;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] bool sbe_mutates_store(std::uint16_t tid) noexcept {
  switch (tid) {
    case kZAdd:
    case kZIncrBy:
    case kZPop:
    case kZRem:
    case kZRemRangeByScore:
    case kSet:
    case kGetSet:
    case kSetNx:
    case kGetDel:
    case kAppend:
    case kIncr:
    case kDecr:
    case kIncrBy:
    case kDecrBy:
    case kIncrByFloat:
    case kDel:
    case kExpire:
    case kPExpire:
    case kExpireAt:
    case kPExpireAt:
    case kPersist:
    case kSetRange:
    case kMSet:
    case kLPush:
    case kRPush:
    case kLPop:
    case kRPop:
    case kLSet:
    case kLTrim:
    case kLRem:
    case kLInsert:
    case kHSet:
    case kHSetNx:
    case kHDel:
    case kHIncrBy:
    case kSAdd:
    case kSRem:
    case kSPop:
    case kSMove:
    case kSInterStore:
    case kSUnionStore:
    case kSDiffStore:
    case kGoblinCad:
    case kGoblinCaExpire:
    case kGoblinCas:
    case kGoblinIncrEx:
    case kGoblinZWindow:
    case kGoblinIncrBound:
    case kGoblinDecrPos:
    case kGoblinHCad:
    case kGoblinHSetGt:
    case kGoblinClaim:
    case kGoblinLoad:
      return true;
    default:
      return false;
  }
}

void replicate_sbe_command(const CommandExecutionOptions& options, Store& store,
                           CommandType type, std::string_view name,
                           std::span<const std::string_view> args) noexcept {
  if (options.replicate_write == nullptr) return;
  const Command command{.type = type, .name = name, .args = args};
  options.replicate_write(options.replication_context, store, command,
                          "+OK\r\n");
}

void replicate_sbe_string(const CommandExecutionOptions& options, Store& store,
                          std::string_view key) noexcept {
  const std::string_view args[]{key};
  replicate_sbe_command(options, store, CommandType::set, "SET", args);
}

void replicate_sbe_ttl(const CommandExecutionOptions& options, Store& store,
                       std::string_view key) noexcept {
  const std::string_view args[]{key};
  replicate_sbe_command(options, store, CommandType::persist, "PERSIST", args);
}

void replicate_sbe_hash_fields(
    const CommandExecutionOptions& options, Store& store, std::string_view key,
    std::span<const std::string_view> fields) noexcept {
  for (const auto field : fields) {
    const std::string_view args[]{key, field, {}};
    replicate_sbe_command(options, store, CommandType::hset, "HSET", args);
  }
}

void replicate_sbe_zset_members(
    const CommandExecutionOptions& options, Store& store, std::string_view key,
    std::span<const std::string_view> members) noexcept {
  for (const auto member : members) {
    const std::string_view args[]{key, "0", member};
    replicate_sbe_command(options, store, CommandType::zadd, "ZADD", args);
  }
}

void replicate_sbe_set_members(
    const CommandExecutionOptions& options, Store& store, std::string_view key,
    std::span<const std::string_view> members) noexcept {
  if (members.empty()) return;
  static thread_local std::vector<std::string_view> args;
  args.clear();
  args.reserve(members.size() + 1);
  args.push_back(key);
  args.insert(args.end(), members.begin(), members.end());
  replicate_sbe_command(options, store, CommandType::sadd, "SADD", args);
}

// The key a type-specific request operates on, for the WRONGTYPE check. For every
// guarded message except the five below, the key is the leading varData -- at
// header + block, encoded as [uint32 length][bytes] (the varData composite) -- so it
// decodes generically without the message class. ZADD/ZREM/HSET/HMGET/HDEL lay the key
// *after* their group; those return nullopt here and are guarded inline in the handler
// (where the key is already decoded past the group). nullopt also for a truncated frame
// (dropped downstream). Never emit a bogus key: a wrong offset would guard the wrong key.
[[nodiscard]] std::optional<std::string_view> sbe_leading_key(
    std::uint16_t tid, const char* buf, std::uint16_t block_length, std::uint64_t buflen) {
  switch (tid) {
    case kZAdd: case kZRem: case kZMScore: case kHSet: case kHMGet: case kHDel:
    case kLPush: case kRPush:
    case kSAdd: case kSRem: case kSMIsMember:
    case kSInter: case kSUnion: case kSDiff:
    case kSInterStore: case kSUnionStore: case kSDiffStore:
    case kSInterCard: case kSMove:
      return std::nullopt;  // key trails a group / multi-key -> guarded inline
    default:
      break;
  }
  const std::size_t pos = static_cast<std::size_t>(kBodyOffset) + block_length;
  if (pos + sizeof(std::uint32_t) > buflen) return std::nullopt;
  std::uint32_t len = 0;
  std::memcpy(&len, buf + pos, sizeof(len));
  if (static_cast<std::uint64_t>(pos) + sizeof(len) + len > buflen) return std::nullopt;
  return std::string_view(buf + pos + sizeof(len), len);
}

[[nodiscard]] bool equals_ci_sbe(std::string_view lhs,
                                 std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto upper = [](char byte) {
      return byte >= 'a' && byte <= 'z'
                 ? static_cast<char>(byte - ('a' - 'A'))
                 : byte;
    };
    if (upper(lhs[index]) != upper(rhs[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] constexpr std::string_view sbe_key_type_name(
    KeyType type) noexcept {
  switch (type) {
    case KeyType::String: return "string";
    case KeyType::Zset: return "zset";
    case KeyType::Hash: return "hash";
    case KeyType::List: return "list";
    case KeyType::Set: return "set";
    case KeyType::Array: return "array";
  }
  return "none";
}

// Redis-style glob for SCAN-family MATCH (* ? [abc]).
[[nodiscard]] bool scan_glob_match_sbe(std::string_view pattern,
                                       std::string_view value) noexcept {
  std::size_t pi = 0;
  std::size_t vi = 0;
  std::size_t star_pi = std::string_view::npos;
  std::size_t star_vi = 0;
  while (vi < value.size()) {
    if (pi < pattern.size() &&
        (pattern[pi] == '?' || pattern[pi] == value[vi])) {
      ++pi;
      ++vi;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == '*') {
      star_pi = pi++;
      star_vi = vi;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == '[') {
      const auto close = pattern.find(']', pi + 1);
      if (close == std::string_view::npos) {
        return false;
      }
      const auto body = pattern.substr(pi + 1, close - pi - 1);
      bool negate = false;
      std::size_t ci = 0;
      if (!body.empty() && (body[0] == '^' || body[0] == '!')) {
        negate = true;
        ci = 1;
      }
      bool matched = false;
      while (ci < body.size()) {
        if (ci + 2 < body.size() && body[ci + 1] == '-') {
          if (value[vi] >= body[ci] && value[vi] <= body[ci + 2]) {
            matched = true;
            break;
          }
          ci += 3;
        } else {
          if (body[ci] == value[vi]) {
            matched = true;
            break;
          }
          ++ci;
        }
      }
      if (matched == negate) {
        if (star_pi != std::string_view::npos) {
          pi = star_pi + 1;
          vi = ++star_vi;
          continue;
        }
        return false;
      }
      pi = close + 1;
      ++vi;
      continue;
    }
    if (star_pi != std::string_view::npos) {
      pi = star_pi + 1;
      vi = ++star_vi;
      continue;
    }
    return false;
  }
  while (pi < pattern.size() && pattern[pi] == '*') {
    ++pi;
  }
  return pi == pattern.size();
}

// Reply WRONGTYPE and return true when `key` exists holding a type other than
// `required` (so the handler must stop); false when it is absent or the right type.
[[nodiscard]] bool wrong_type(Store& store, KeyType required, std::string_view key,
                              std::string& out) {
  if (const auto actual = store.key_type(key); actual && *actual != required) {
    reply_error(out, "WRONGTYPE", kWrongTypeMsg);
    return true;
  }
  return false;
}

[[nodiscard]] bool decode_list_implementation(
    std::uint8_t wire, std::optional<ListImplementation>& implementation,
    std::string& out) {
  switch (wire) {
    case 0:
      implementation.reset();
      return true;
    case 1:
      implementation = ListImplementation::Pma;
      return true;
    case 2:
      implementation = ListImplementation::Segmented;
      return true;
    default:
      reply_error(out, "ERR", "invalid list implementation");
      return false;
  }
}

void handle(Store& store, std::uint16_t tid, char* buf, std::uint64_t buflen,
            std::uint16_t block_length, std::uint16_t version, std::string& out,
            const CommandExecutionOptions& options) {
  if (options.read_only && sbe_mutates_store(tid)) {
    reply_error(out, "READONLY",
                "You can't write against a read-only replica.");
    return;
  }
  if (tid == kGoblinLoad && options.replicate_write != nullptr) {
    reply_error(
        out, "ERR",
        "GOBLIN.LOAD is disabled on a running replicated server; use --load at startup");
    return;
  }
  // Reject a command aimed at the wrong key type before it reaches a typed store
  // accessor (see sbe_requires_type). The five group-then-key messages carry the key
  // after their group, so they self-check inside their case with wrong_type().
  if (const auto required = sbe_requires_type(tid)) {
    if (const auto key = sbe_leading_key(tid, buf, block_length, buflen)) {
      if (wrong_type(store, *required, *key, out)) return;
    }
  }

  switch (tid) {
    case kPing:
      reply<sbe::StatusReply>(out, [](sbe::StatusReply& r) { r.putStatus("PONG", 4); });
      break;

    case kZAdd: {
      sbe::ZAdd z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto flags = z.flags();
      constexpr std::uint8_t kNx = 0x01;
      constexpr std::uint8_t kXx = 0x02;
      constexpr std::uint8_t kGt = 0x04;
      constexpr std::uint8_t kLt = 0x08;
      constexpr std::uint8_t kCh = 0x10;
      constexpr std::uint8_t kIncr = 0x20;
      const ZAddOptions add_options{
          .nx = (flags & kNx) != 0,
          .xx = (flags & kXx) != 0,
          .gt = (flags & kGt) != 0,
          .lt = (flags & kLt) != 0,
          .increment = (flags & kIncr) != 0,
      };
      if ((flags & ~(kNx | kXx | kGt | kLt | kCh | kIncr)) != 0 ||
          (add_options.nx &&
           (add_options.xx || add_options.gt || add_options.lt)) ||
          (add_options.gt && add_options.lt)) {
        reply_error(out, "ERR", kErrSyntax);
        break;
      }
      // SBE lays the trailing key after the group, so buffer the (score, member)
      // pairs in one pass (member views stay valid), then read the key and apply.
      static thread_local std::vector<ZAddItem> members;
      members.clear();
      bool invalid_score = false;
      auto& group = z.members();
      while (group.hasNext()) {
        group.next();
        const auto score = group.score();
        if (std::isnan(score)) {
          reply_error(out, "ERR", kErrNotFloat);
          invalid_score = true;
          break;
        }
        members.push_back(
            ZAddItem{.score = score, .member = group.getMemberAsStringView()});
      }
      if (invalid_score) break;
      const std::string_view key = z.getKeyAsStringView();  // key trails the group
      if (members.empty() || (add_options.increment && members.size() != 1)) {
        reply_error(out, "ERR", kErrSyntax);
        break;
      }
      if (wrong_type(store, KeyType::Zset, key, out)) break;
      const auto result = store.zadd(key, members, add_options);
      if (result.invalid_score) {
        reply_error(out, "ERR", "resulting score is not a number (NaN)");
      } else if (add_options.increment) {
        if (result.increment_score) reply_double(out, *result.increment_score);
        else reply_nil(out);
      } else {
        reply_int(out, (flags & kCh) != 0 ? result.changed : result.added);
      }
      if (!result.invalid_score) {
        for (const auto& member : members) {
          const std::string_view changed[]{member.member};
          replicate_sbe_zset_members(options, store, key, changed);
        }
      }
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

    case kZIncrBy: {
      sbe::ZIncrBy z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto increment = z.increment();
      if (std::isnan(increment)) {
        reply_error(out, "ERR", kErrNotFloat);
        break;
      }
      const auto key = z.getKeyAsStringView();
      const auto member = z.getMemberAsStringView();
      const ZAddItem item{.score = increment, .member = member};
      const auto result = store.zadd(
          key, std::span<const ZAddItem>(&item, 1),
          ZAddOptions{.increment = true});
      if (result.invalid_score) {
        reply_error(out, "ERR", "resulting score is not a number (NaN)");
      } else {
        reply_double(out, *result.increment_score);
        const std::string_view changed[]{member};
        replicate_sbe_zset_members(options, store, key, changed);
      }
      break;
    }

    case kZRangeByScore: {
      sbe::ZRangeByScore z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto min = z.min();
      const auto max = z.max();
      const auto offset = z.limitOffset();
      const auto limit_count = z.limitCount();
      if (std::isnan(min) || std::isnan(max)) {
        reply_error(out, "ERR", "min or max is not a float");
        break;
      }
      if (limit_count < -1 ||
          offset > std::numeric_limits<std::size_t>::max()) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const bool reverse = z.reverse() != 0;
      const bool with_scores = z.withScores() != 0;
      const bool min_exclusive = z.minExclusive() != 0;
      const bool max_exclusive = z.maxExclusive() != 0;
      const auto key = z.getKeyAsStringView();
      const auto limit = limit_count < 0
                             ? std::optional<std::size_t>{}
                             : std::optional<std::size_t>{
                                   static_cast<std::size_t>(limit_count)};
      if (with_scores) {
        static thread_local std::vector<std::pair<std::string_view, double>>
            items;
        items.clear();
        store.zrange_by_score_values_for_each_counted(
            key, min, min_exclusive, max, max_exclusive, reverse,
            static_cast<std::size_t>(offset), limit,
            [&](std::size_t count) { items.reserve(count); },
            [&](std::string_view member, double score) {
              items.emplace_back(member, score);
            });
        reply_scored_array(out, items);
      } else {
        static thread_local std::vector<std::string_view> members;
        members.clear();
        store.zrange_by_score_members_for_each_counted(
            key, min, min_exclusive, max, max_exclusive, reverse,
            static_cast<std::size_t>(offset), limit,
            [&](std::size_t count) { members.reserve(count); },
            [&](std::string_view member) { members.push_back(member); });
        reply_array(out, members);
      }
      break;
    }

    case kZCount: {
      sbe::ZCount z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      if (std::isnan(z.min()) || std::isnan(z.max())) {
        reply_error(out, "ERR", "min or max is not a float");
        break;
      }
      reply_int(out, store.zcount(z.getKeyAsStringView(), z.min(),
                                  z.minExclusive() != 0, z.max(),
                                  z.maxExclusive() != 0));
      break;
    }

    case kZMScore: {
      sbe::ZMScore z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> members;
      members.clear();
      auto& group = z.members();
      while (group.hasNext()) {
        group.next();
        members.push_back(group.getMemberAsStringView());
      }
      const auto key = z.getKeyAsStringView();
      if (members.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for 'zmscore' command");
        break;
      }
      if (wrong_type(store, KeyType::Zset, key, out)) break;
      static thread_local std::vector<std::optional<double>> scores;
      scores.clear();
      scores.reserve(members.size());
      store.zmscore_for_each(key, members,
                             [](std::optional<double> score) {
                               scores.push_back(score);
                             });
      reply_nullable_double_array(out, scores);
      break;
    }

    case kZPop: {
      sbe::ZPop z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      if (z.count() > std::numeric_limits<std::size_t>::max()) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto key = z.getKeyAsStringView();
      const auto popped =
          store.zpop(key, static_cast<std::size_t>(z.count()),
                     z.maximum() != 0);
      static thread_local std::vector<std::pair<std::string_view, double>> items;
      items.clear();
      items.reserve(popped.size());
      for (const auto& item : popped) {
        items.emplace_back(item.member, item.score);
      }
      reply_scored_array(out, items);
      for (const auto& item : popped) {
        const std::string_view changed[]{item.member};
        replicate_sbe_zset_members(options, store, key, changed);
      }
      break;
    }

    case kZScan: {
      sbe::ZScan z;
      z.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      if (z.count() == 0 ||
          z.count() > std::numeric_limits<std::size_t>::max()) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto key = z.getKeyAsStringView();
      const auto pattern = z.getMatchAsStringView();
      const bool has_match = z.hasMatch() != 0;
      static thread_local std::vector<std::pair<std::string_view, double>> items;
      items.clear();
      const auto next = store.zscan(
          key, z.cursor(), static_cast<std::size_t>(z.count()),
          [&](std::string_view member, double score) {
            if (!has_match || scan_glob_match_sbe(pattern, member)) {
              items.emplace_back(member, score);
            }
          });
      reply_scored_scan(out, next, items);
      break;
    }

    case kGet: {
      sbe::Get g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      // One keyspace probe (kGet omitted from sbe_requires_type).
      const auto result = store.get_result(g.getKeyAsStringView());
      switch (result.status) {
        case StringLookup::Missing:
          reply_nil(out);
          break;
        case StringLookup::WrongType:
          reply_error(out, "WRONGTYPE", kWrongTypeMsg);
          break;
        case StringLookup::Ok:
          reply_bulk_value(out, result.value);
          break;
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

      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      // Common case: plain SET with no NX/XX/GET/KEEPTTL/expire.
      if (flags == 0 && expire_mode == 0) {
        store.set(key, value);
        reply<sbe::StatusReply>(out, [](sbe::StatusReply& r) {
          r.putStatus("OK", 2);
        });
        replicate_sbe_string(options, store, key);
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
          current->append_to(*old_value);
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
      if (condition_met) replicate_sbe_string(options, store, key);
      break;
    }

    case kGetSet: {
      sbe::GetSet g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view value = g.getValueAsStringView();
      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_bulk_or_nil(out, store.get_set(key, value));
      replicate_sbe_string(options, store, key);
      break;
    }

    case kSetNx: {
      sbe::SetNx g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view value = g.getValueAsStringView();
      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const bool changed = store.set_nx(key, value);
      reply_int(out, changed ? 1 : 0);
      if (changed) replicate_sbe_string(options, store, key);
      break;
    }

    case kGetDel: {
      sbe::GetDel g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = g.getKeyAsStringView();
      const auto removed = store.get_del(key);
      reply_bulk_or_nil(out, removed);
      if (removed) replicate_sbe_string(options, store, key);
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
      const auto length = store.append(key, value);
      if (!length) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_int(out, static_cast<long long>(*length));
      replicate_sbe_string(options, store, key);
      break;
    }

    case kIncr: {
      sbe::Incr g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_by(key, 1);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kDecr: {
      sbe::Decr g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_by(key, -1);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kIncrBy: {
      sbe::IncrBy g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::int64_t delta = g.delta();
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_by(key, delta);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
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
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_by(key, -delta);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kIncrByFloat: {
      sbe::IncrByFloat g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double delta = g.delta();
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_by_float(key, delta);
      if (result) {
        reply_bulk(out, *result);  // formatted float, matches RESP
        replicate_sbe_string(options, store, key);
      } else {
        reply_error(out, "ERR", kErrNotFloat);
      }
      break;
    }

    case kDel: {
      sbe::Del d;
      d.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> removed_keys;
      removed_keys.clear();
      long long removed = 0;
      auto& keys = d.keys();
      while (keys.hasNext()) {
        keys.next();
        const auto key = keys.getKeyAsStringView();
        removed_keys.push_back(key);
        removed += store.del(key) ? 1 : 0;
      }
      reply_int(out, removed);
      replicate_sbe_command(options, store, CommandType::del, "DEL",
                            removed_keys);
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

    case kScan: {
      sbe::Scan scan;
      scan.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      if (scan.count() == 0 ||
          scan.count() > std::numeric_limits<std::size_t>::max()) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto pattern = scan.getMatchAsStringView();
      const auto type_filter = scan.getTypeAsStringView();
      const bool has_match = scan.hasMatch() != 0;
      const bool has_type = scan.hasType() != 0;
      static thread_local std::vector<std::string_view> items;
      items.clear();
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      const auto next = store.scan(
          scan.cursor(), static_cast<std::size_t>(scan.count()), now,
          [&](std::string_view key, KeyType type) {
            if ((!has_match || scan_glob_match_sbe(pattern, key)) &&
                (!has_type ||
                 equals_ci_sbe(type_filter, sbe_key_type_name(type)))) {
              items.push_back(key);
            }
          });
      reply_string_scan(out, next, items);
      break;
    }

    case kType: {
      sbe::Type t;
      t.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto type = store.key_type(t.getKeyAsStringView());
      reply_status(out, type ? sbe_key_type_name(*type) : "none");
      break;
    }

    case kExpire: {
      sbe::Expire e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      const auto key = e.getKeyAsStringView();
      handle_expire(store, true, 1000, amount, flags, key, out);
      replicate_sbe_ttl(options, store, key);
      break;
    }
    case kPExpire: {
      sbe::PExpire e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      const auto key = e.getKeyAsStringView();
      handle_expire(store, true, 1, amount, flags, key, out);
      replicate_sbe_ttl(options, store, key);
      break;
    }
    case kExpireAt: {
      sbe::ExpireAt e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      const auto key = e.getKeyAsStringView();
      handle_expire(store, false, 1000, amount, flags, key, out);
      replicate_sbe_ttl(options, store, key);
      break;
    }
    case kPExpireAt: {
      sbe::PExpireAt e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long amount = e.amount();
      const unsigned flags = e.flags();
      const auto key = e.getKeyAsStringView();
      handle_expire(store, false, 1, amount, flags, key, out);
      replicate_sbe_ttl(options, store, key);
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
      const auto key = p.getKeyAsStringView();
      reply_int(out, store.persist(key) ? 1 : 0);
      replicate_sbe_ttl(options, store, key);
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

    case kLPush:
    case kRPush: {
      static thread_local std::vector<std::string_view> values;
      values.clear();
      bool too_big = false;
      std::string_view key;
      std::uint8_t implementation_wire = 0;
      bool only_if_exists = false;

      if (tid == kLPush) {
        sbe::LPush command;
        command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        implementation_wire = command.implementation();
        only_if_exists = command.onlyIfExists() != 0;
        auto& group = command.values();
        while (group.hasNext()) {
          group.next();
          const auto value = group.getValueAsStringView();
          too_big = too_big || !store.value_fits(value);
          values.push_back(value);
        }
        key = command.getKeyAsStringView();
      } else {
        sbe::RPush command;
        command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        implementation_wire = command.implementation();
        only_if_exists = command.onlyIfExists() != 0;
        auto& group = command.values();
        while (group.hasNext()) {
          group.next();
          const auto value = group.getValueAsStringView();
          too_big = too_big || !store.value_fits(value);
          values.push_back(value);
        }
        key = command.getKeyAsStringView();
      }

      if (wrong_type(store, KeyType::List, key, out)) break;
      if (values.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for list push command");
        break;
      }
      if (too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      std::optional<ListImplementation> implementation;
      if (!decode_list_implementation(implementation_wire, implementation, out)) break;
      const auto length = tid == kLPush
                              ? store.lpush(key, values, only_if_exists, implementation)
                              : store.rpush(key, values, only_if_exists, implementation);
      reply_int(out, length);
      static thread_local std::vector<std::string_view> replication_args;
      replication_args.clear();
      replication_args.reserve(values.size() + 1);
      replication_args.push_back(key);
      replication_args.insert(replication_args.end(), values.begin(), values.end());
      if (tid == kLPush) {
        replicate_sbe_command(options, store,
                              only_if_exists ? CommandType::lpushx
                                             : CommandType::lpush,
                              only_if_exists ? "LPUSHX" : "LPUSH",
                              replication_args);
      } else {
        replicate_sbe_command(options, store,
                              only_if_exists ? CommandType::rpushx
                                             : CommandType::rpush,
                              only_if_exists ? "RPUSHX" : "RPUSH",
                              replication_args);
      }
      break;
    }

    case kLPop:
    case kRPop: {
      long long count = -1;
      std::string_view key;
      if (tid == kLPop) {
        sbe::LPop command;
        command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        count = command.count();
        key = command.getKeyAsStringView();
      } else {
        sbe::RPop command;
        command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        count = command.count();
        key = command.getKeyAsStringView();
      }
      if (count == -1) {
        reply_bulk_or_nil(out, tid == kLPop ? store.lpop(key) : store.rpop(key));
        const std::string_view args[]{key};
        replicate_sbe_command(options, store,
                              tid == kLPop ? CommandType::lpop
                                           : CommandType::rpop,
                              tid == kLPop ? "LPOP" : "RPOP", args);
        break;
      }
      if (count < 0) {
        reply_error(out, "ERR", "value is out of range, must be positive");
        break;
      }
      if (!store.exists(key)) {
        reply_nil(out);
        break;
      }
      const auto values = tid == kLPop
                              ? store.lpop(key, static_cast<std::size_t>(count))
                              : store.rpop(key, static_cast<std::size_t>(count));
      reply_array(out, values);
      const std::string count_text = std::to_string(count);
      const std::string_view args[]{key, count_text};
      replicate_sbe_command(options, store,
                            tid == kLPop ? CommandType::lpop
                                         : CommandType::rpop,
                            tid == kLPop ? "LPOP" : "RPOP", args);
      break;
    }

    case kLLen: {
      sbe::LLen command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, static_cast<long long>(store.llen(command.getKeyAsStringView())));
      break;
    }

    case kLIndex: {
      sbe::LIndex command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto value = store.lindex(command.getKeyAsStringView(), command.index());
      if (value) {
        reply_bulk(out, value->to_string());
      } else {
        reply_nil(out);
      }
      break;
    }

    case kLRange: {
      sbe::LRange command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto encoded = store.lrange(command.getKeyAsStringView(), command.start(),
                                        command.stop());
      static thread_local std::vector<std::string> values;
      values.clear();
      values.reserve(encoded.size());
      for (const auto value : encoded) values.push_back(value.to_string());
      reply_array(out, values);
      break;
    }

    case kLSet: {
      sbe::LSet command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = command.getKeyAsStringView();
      const auto value = command.getValueAsStringView();
      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const auto result = store.lset(key, command.index(), value);
      switch (result) {
        case Store::ListSetResult::Stored:
          reply_status(out, "OK");
          break;
        case Store::ListSetResult::MissingKey:
          reply_error(out, "ERR", "no such key");
          break;
        case Store::ListSetResult::OutOfRange:
          reply_error(out, "ERR", "index out of range");
          break;
      }
      if (result == Store::ListSetResult::Stored) {
        const std::string index = std::to_string(command.index());
        const std::string_view args[]{key, index, value};
        replicate_sbe_command(options, store, CommandType::lset, "LSET", args);
      }
      break;
    }

    case kLTrim: {
      sbe::LTrim command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = command.getKeyAsStringView();
      store.ltrim(key, command.start(), command.stop());
      reply_status(out, "OK");
      const std::string start = std::to_string(command.start());
      const std::string stop = std::to_string(command.stop());
      const std::string_view args[]{key, start, stop};
      replicate_sbe_command(options, store, CommandType::ltrim, "LTRIM", args);
      break;
    }

    case kLRem: {
      sbe::LRem command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = command.getKeyAsStringView();
      const auto value = command.getValueAsStringView();
      reply_int(out, static_cast<long long>(store.lrem(key, command.count(), value)));
      const std::string count = std::to_string(command.count());
      const std::string_view args[]{key, count, value};
      replicate_sbe_command(options, store, CommandType::lrem, "LREM", args);
      break;
    }

    case kLInsert: {
      sbe::LInsert command;
      command.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = command.getKeyAsStringView();
      const auto pivot = command.getPivotAsStringView();
      const auto value = command.getValueAsStringView();
      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const bool before = command.before() != 0;
      reply_int(out, store.linsert(key, before, pivot, value));
      const std::string_view args[]{key, before ? "BEFORE" : "AFTER", pivot,
                                    value};
      replicate_sbe_command(options, store, CommandType::linsert, "LINSERT",
                            args);
      break;
    }

    case kHSet: {
      sbe::HSet h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      // Key trails the entries group on the wire, so we must scan entries first.
      static thread_local std::vector<std::pair<std::string_view, std::string_view>> entries;
      entries.clear();
      bool too_big = false;
      auto& g = h.entries();
      while (g.hasNext()) {
        g.next();
        const auto f = g.getFieldAsStringView();
        const auto v = g.getValueAsStringView();
        if (f.size() > HashStorage::kMaxFieldBytes ||
            !store.value_fits(v)) {
          too_big = true;
        }
        entries.emplace_back(f, v);
      }
      const std::string_view key = h.getKeyAsStringView();
      // Fuse WRONGTYPE with the create path: one keyspace probe. wrong_type()
      // only rejects non-hash existing keys; absent keys fall through to
      // hset_many → get_or_create_hash.
      if (wrong_type(store, KeyType::Hash, key, out)) break;
      if (too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      if (entries.size() == 1) {
        reply_int(out, store.hset(key, entries.front().first,
                                 entries.front().second));
      } else {
        reply_int(out, store.hset_many(key, entries));
      }
      static thread_local std::vector<std::string_view> fields;
      fields.clear();
      fields.reserve(entries.size());
      for (const auto& [field, value] : entries) {
        (void)value;
        fields.push_back(field);
      }
      replicate_sbe_hash_fields(options, store, key, fields);
      break;
    }

    case kHSetNx: {
      sbe::HSetNx h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      const std::string_view value = h.getValueAsStringView();
      if (field.size() > HashStorage::kMaxFieldBytes ||
          !store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const bool changed = store.hsetnx(key, field, value);
      reply_int(out, changed ? 1 : 0);
      if (changed) {
        const std::string_view fields[]{field};
        replicate_sbe_hash_fields(options, store, key, fields);
      }
      break;
    }

    case kHGet: {
      sbe::HGet h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      const std::string_view field = h.getFieldAsStringView();
      const auto v = store.hget(key, field);
      if (v) {
        reply_bulk_value(out, *v);
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
      const std::string_view key = h.getKeyAsStringView();  // key trails the group
      if (wrong_type(store, KeyType::Hash, key, out)) break;
      // One keyspace probe; keep decoded strings only when the stored form is
      // not a raw contiguous slice (reply_nullable_array accepts string_view).
      static thread_local std::vector<std::optional<std::string>> owned;
      static thread_local std::vector<std::optional<std::string_view>> values;
      owned.clear();
      values.clear();
      owned.reserve(fields.size());
      values.reserve(fields.size());
      store.hash_mget(key, fields, [&](std::optional<EncodedStringView> value) {
        if (!value) {
          values.push_back(std::nullopt);
          return;
        }
        if (value->is_raw() && value->encoded_tail().empty() &&
            value->encoded_head().size() >= (value->encoding_enabled() ? 1 : 0)) {
          if (value->encoding_enabled()) {
            values.push_back(std::string_view(value->encoded_head().data() + 1,
                                              value->encoded_head().size() - 1));
          } else {
            values.push_back(value->encoded_head());
          }
          return;
        }
        owned.push_back(value->to_string());
        values.push_back(std::string_view(*owned.back()));
      });
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
      const std::string_view key = h.getKeyAsStringView();  // key trails the group
      if (wrong_type(store, KeyType::Hash, key, out)) break;
      reply_int(out, static_cast<long long>(store.hdel_many(key, fields)));
      replicate_sbe_hash_fields(options, store, key, fields);
      break;
    }

    case kHGetAll: {
      sbe::HGetAll h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::string> flat;
      flat.clear();
      store.hash_for_each(key, [&](std::string_view f, EncodedStringView v) {
        flat.emplace_back(f);
        flat.push_back(v.to_string());
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
      store.hash_for_each(key, [&](std::string_view f, EncodedStringView) {
        keys.push_back(f);
      });
      reply_array(out, keys);
      break;
    }

    case kHVals: {
      sbe::HVals h;
      h.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = h.getKeyAsStringView();
      static thread_local std::vector<std::string> vals;
      vals.clear();
      store.hash_for_each(key, [&](std::string_view, EncodedStringView v) {
        vals.push_back(v.to_string());
      });
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
        const std::string_view fields[]{field};
        replicate_sbe_hash_fields(options, store, key, fields);
      } else {
        reply_error(out, "ERR", "hash value is not an integer or out of range");
      }
      break;
    }

    case kHScan: {
      sbe::HScan scan;
      scan.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      if (scan.count() == 0 ||
          scan.count() > std::numeric_limits<std::size_t>::max()) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto key = scan.getKeyAsStringView();
      const auto pattern = scan.getMatchAsStringView();
      const bool has_match = scan.hasMatch() != 0;
      const bool no_values = scan.noValues() != 0;
      static thread_local std::vector<std::string> items;
      items.clear();
      const auto next = store.hscan(
          key, scan.cursor(), static_cast<std::size_t>(scan.count()),
          [&](std::string_view field) {
            return !has_match || scan_glob_match_sbe(pattern, field);
          },
          [&](std::string_view field, EncodedStringView value) {
            items.emplace_back(field);
            if (!no_values) {
              items.push_back(value.to_string());
            }
          });
      reply_string_scan(out, next, items);
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
      const std::string_view key = z.getKeyAsStringView();  // key trails the group
      if (wrong_type(store, KeyType::Zset, key, out)) break;
      reply_int(out, store.zrem(key, std::span<const std::string_view>(members)));
      replicate_sbe_zset_members(options, store, key, members);
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
      std::string min_text = format_score(min);
      std::string max_text = format_score(max);
      if (min_excl) min_text.insert(min_text.begin(), '(');
      if (max_excl) max_text.insert(max_text.begin(), '(');
      const std::string_view args[]{key, min_text, max_text};
      replicate_sbe_command(options, store, CommandType::zremrangebyscore,
                            "ZREMRANGEBYSCORE", args);
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
        replicate_sbe_string(options, store, key);
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
        if (!store.value_fits(v)) too_big = true;
        pairs.emplace_back(k, v);
      }
      if (too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      for (const auto& [k, v] : pairs) store.set(k, v);
      reply_status(out, "OK");
      for (const auto& [key, value] : pairs) {
        (void)value;
        replicate_sbe_string(options, store, key);
      }
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
          v->append_to(s);
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

    case kGoblinCad: {
      sbe::GoblinCad g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view token = g.getTokenAsStringView();
      const bool changed = store.compare_and_delete(key, token);
      reply_int(out, changed ? 1 : 0);
      if (changed) replicate_sbe_string(options, store, key);
      break;
    }

    case kGoblinCaExpire: {
      sbe::GoblinCaExpire g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long ms = g.ms();
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view token = g.getTokenAsStringView();
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, ms, 1);
      if (!when) {
        reply_error(out, "ERR", "invalid expire time");
        break;
      }
      const bool changed = store.compare_and_expire(key, token, *when, now);
      reply_int(out, changed ? 1 : 0);
      if (changed) replicate_sbe_ttl(options, store, key);
      break;
    }

    case kGoblinCas: {
      sbe::GoblinCas g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view token = g.getTokenAsStringView();
      const std::string_view value = g.getValueAsStringView();
      if (!store.value_fits(value)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      if (store.compare_and_set(key, token, value)) {
        reply_status(out, "OK");  // KEEPTTL swap
        replicate_sbe_string(options, store, key);
      } else {
        reply_int(out, 0);  // token did not match
      }
      break;
    }

    case kGoblinTdRescore: {
      sbe::GoblinTdRescore t;
      t.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double now_v = t.now();
      const double hl = t.hl();
      const long long k_parsed = t.k();
      const std::uint8_t mode = t.mode();  // 0 LINEAR, 1 EXP, 2 STEP
      const std::string_view key = t.getKeyAsStringView();
      if (mode > 2) {
        reply_error(out, "ERR", "mode must be LINEAR, EXP or STEP");
        break;
      }
      const double inv = 1.0 / hl;
      const double cutoff = now_v - hl;
      const std::size_t k = k_parsed > 0 ? static_cast<std::size_t>(k_parsed) : 0;
      // Bounded top-k kept sorted descending (same insertion sort as command.cpp),
      // holding member views straight from the store -- no per-entry copy. Native
      // decayed weights ride back in the ScoredArrayReply, no restringify.
      static thread_local std::vector<std::pair<std::string_view, double>> best;
      best.clear();
      best.reserve(std::min<std::size_t>(k, 4096));
      const auto push = [k](std::string_view name, double s) {  // `best` is static: use it directly, don't capture
        std::size_t j = 0;
        if (best.size() < k) {
          best.emplace_back(name, s);
          j = best.size() - 1;
        } else if (k > 0 && s > best[k - 1].second) {
          best[k - 1] = {name, s};
          j = k - 1;
        } else {
          return;
        }
        while (j > 0 && best[j].second > best[j - 1].second) {
          std::swap(best[j], best[j - 1]);
          --j;
        }
      };
      if (k > 0) {
        store.for_each_zset_entry(key, [&](std::string_view member, double score) {
          const double age = now_v - score;
          double decayed;
          switch (mode) {
            case 0: decayed = 1.0 / (1.0 + age * inv); break;      // LINEAR
            case 1: decayed = std::pow(0.5, age * inv); break;     // EXP
            default: decayed = score >= cutoff ? 1.0 : 0.0; break;  // STEP
          }
          push(member, decayed);
        });
      }
      reply_scored_array(out, best);
      break;
    }

    case kGoblinIncrEx: {
      sbe::GoblinIncrEx g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long seconds = g.seconds();
      const std::string_view key = g.getKeyAsStringView();
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, seconds, 1000);
      if (!when) {
        reply_error(out, "ERR", "invalid expire time");
        break;
      }
      const auto result = store.incr_expire(key, *when, now);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kGoblinZWindow: {
      sbe::GoblinZWindow g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double now_v = g.now();
      const double window = g.window();
      const long long limit = g.limit();
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view member = g.getMemberAsStringView();
      if (window > 9.0e15 || window < -9.0e15) {
        reply_error(out, "ERR", "invalid expire time");
        break;
      }
      const std::uint64_t clock = store.now_ms();
      const auto when = compute_when_ms(clock, static_cast<long long>(window), 1000);
      if (!when) {
        reply_error(out, "ERR", "invalid expire time");
        break;
      }
      const double cutoff = now_v - window;
      const bool changed =
          store.zwindow(key, now_v, cutoff, limit, member, *when, clock);
      reply_int(out, changed ? 1 : 0);
      const std::string now_text = format_score(now_v);
      const std::string window_text = format_score(window);
      const std::string limit_text = std::to_string(limit);
      const std::string_view args[]{key, now_text, window_text, limit_text,
                                    member};
      replicate_sbe_command(options, store, CommandType::goblin_zwindow,
                            "GOBLIN.ZWINDOW", args);
      replicate_sbe_ttl(options, store, key);
      break;
    }

    case kGoblinIncrBound: {
      sbe::GoblinIncrBound g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long delta = g.delta();
      const long long max = g.max();
      const auto key = g.getKeyAsStringView();
      const auto result = store.incr_bound(key, delta, max);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kGoblinDecrPos: {
      sbe::GoblinDecrPos g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto key = g.getKeyAsStringView();
      const auto result = store.decr_positive(key);
      reply_int_or_range_error(out, result);
      if (result) replicate_sbe_string(options, store, key);
      break;
    }

    case kGoblinHCad: {
      sbe::GoblinHCad g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view field = g.getFieldAsStringView();
      const std::string_view expected = g.getExpectedAsStringView();
      const bool changed = store.hash_compare_and_delete(key, field, expected);
      reply_int(out, changed ? 1 : 0);
      if (changed) {
        const std::string_view fields[]{field};
        replicate_sbe_hash_fields(options, store, key, fields);
      }
      break;
    }

    case kGoblinHSetGt: {
      sbe::GoblinHSetGt g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::string_view key = g.getKeyAsStringView();
      const std::string_view field = g.getFieldAsStringView();
      const std::string_view value = g.getValueAsStringView();
      double parsed = 0;
      const char* first = value.data();
      const char* last = first + value.size();
      const auto res = std::from_chars(first, last, parsed);
      if (res.ptr != last || res.ec == std::errc::invalid_argument) {
        reply_error(out, "ERR", kErrNotFloat);
        break;
      }
      const auto result = store.hash_set_if_greater(key, field, parsed, value);
      if (!result) {
        reply_error(out, "ERR", "hash value is not a float");
        break;
      }
      reply_int(out, *result ? 1 : 0);
      if (*result) {
        const std::string_view fields[]{field};
        replicate_sbe_hash_fields(options, store, key, fields);
      }
      break;
    }

    case kGoblinClaim: {
      sbe::GoblinClaim g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const long long seconds = g.seconds();
      const std::string_view claim_key = g.getClaimKeyAsStringView();
      const std::string_view result_key = g.getResultKeyAsStringView();
      const std::string_view token = g.getTokenAsStringView();
      if (seconds <= 0) {
        reply_error(out, "ERR", "invalid expire time in 'goblin.claim' command");
        break;
      }
      if (!store.value_fits(token)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, seconds, 1000);
      if (!when) {
        reply_error(out, "ERR", "invalid expire time in 'goblin.claim' command");
        break;
      }
      const auto outcome = store.claim(claim_key, result_key, token, *when, now);
      if (outcome.claimed) {
        reply_bulk(out, "CLAIMED");
        replicate_sbe_string(options, store, claim_key);
      } else if (outcome.result_wrongtype) {
        reply_error(out, "WRONGTYPE", kWrongTypeMsg);
      } else if (outcome.result) {
        reply_bulk(out, *outcome.result);
      } else {
        reply_nil(out);
      }
      break;
    }

    case kGoblinOptimize: {
      sbe::GoblinOptimize g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const double density = g.density();
      const std::string_view key = g.getKeyAsStringView();
      // density <= 0 means "omitted" -> the default; anything above 1 is invalid.
      const double d = density <= 0.0 ? kDefaultMemberIndexDensity : density;
      if (d > 1.0) {
        reply_error(out, "ERR", "packing density must be in (0, 1]");
        break;
      }
      const auto reclaimed = store.optimize(key, d);
      if (reclaimed) {
        reply_int(out, static_cast<long long>(*reclaimed));
      } else {
        reply_nil(out);
      }
      break;
    }

    case kInfo:
      reply_bulk(out, render_server_info(store));
      break;

    case kEval: {
      sbe::Eval e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::uint8_t lang = e.language();
      static thread_local std::vector<std::string_view> keys, argv;
      keys.clear();
      argv.clear();
      auto& kg = e.keys();
      while (kg.hasNext()) { kg.next(); keys.push_back(kg.getKeyAsStringView()); }
      auto& ag = e.args();
      while (ag.hasNext()) { ag.next(); argv.push_back(ag.getArgAsStringView()); }
      const std::string_view script = e.getScriptAsStringView();
      eval_dispatch(options, lang, ScriptOp::eval, script, keys, argv, out);
      break;
    }

    case kEvalSha: {
      sbe::EvalSha e;
      e.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::uint8_t lang = e.language();
      static thread_local std::vector<std::string_view> keys, argv;
      keys.clear();
      argv.clear();
      auto& kg = e.keys();
      while (kg.hasNext()) { kg.next(); keys.push_back(kg.getKeyAsStringView()); }
      auto& ag = e.args();
      while (ag.hasNext()) { ag.next(); argv.push_back(ag.getArgAsStringView()); }
      const std::string_view sha = e.getShaAsStringView();
      eval_dispatch(options, lang, ScriptOp::eval_sha, sha, keys, argv, out);
      break;
    }

    case kScript: {
      sbe::Script s;
      s.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const std::uint8_t lang = s.language();
      static thread_local std::vector<std::string_view> sargs;
      sargs.clear();
      auto& ag = s.args();
      while (ag.hasNext()) { ag.next(); sargs.push_back(ag.getArgAsStringView()); }
      run_and_reply(options, lang, ScriptOp::script, sargs, out);
      break;
    }

    case kGoblinMemory: {
      sbe::GoblinMemory g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto fields = goblin_memory_fields(store, g.getKeyAsStringView());
      if (fields) {
        reply_map(out, *fields);
      } else {
        reply_nil(out);
      }
      break;
    }

    case kGoblinSave: {
      sbe::GoblinSave g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const bool accel = g.accel() != 0;
      const std::string_view path = g.getPathAsStringView();
      switch (store.start_background_save(std::string(path), accel)) {
        case Store::SaveStart::Started:
          reply_status(out, "Background saving started");
          break;
        case Store::SaveStart::AlreadyRunning:
          reply_error(out, "ERR", "background save already in progress");
          break;
        case Store::SaveStart::ForkFailed:
          reply_error(out, "ERR", "cannot fork for background save");
          break;
      }
      break;
    }

    case kGoblinLoad: {
      sbe::GoblinLoad g;
      g.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      std::ifstream file(std::string(g.getPathAsStringView()), std::ios::binary);
      if (!file) {
        reply_error(out, "ERR", "cannot open snapshot file for reading");
        break;
      }
      try {
        const auto stats = store.load(file);
        reply_int(out, static_cast<long long>(stats.keys));
      } catch (const std::exception& error) {
        const std::string msg = std::string("snapshot load failed: ") + error.what();
        reply_error(out, "ERR", msg);
      }
      break;
    }

    // ---- Sets ----------------------------------------------------------------
    case kSAdd:
    case kSRem: {
      static thread_local std::vector<std::string_view> members;
      members.clear();
      std::string_view key;
      bool too_big = false;
      if (tid == kSAdd) {
        sbe::SAdd m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        auto& g = m.members();
        while (g.hasNext()) {
          g.next();
          const auto member = g.getMemberAsStringView();
          too_big = too_big || !store.value_fits(member);
          members.push_back(member);
        }
        key = m.getKeyAsStringView();
      } else {
        sbe::SRem m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        auto& g = m.members();
        while (g.hasNext()) {
          g.next();
          members.push_back(g.getMemberAsStringView());
        }
        key = m.getKeyAsStringView();
      }
      if (wrong_type(store, KeyType::Set, key, out)) break;
      if (members.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for set command");
        break;
      }
      if (tid == kSAdd && too_big) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      reply_int(out, tid == kSAdd ? store.sadd(key, members)
                                  : store.srem(key, members));
      replicate_sbe_set_members(options, store, key, members);
      break;
    }

    case kSCard: {
      sbe::SCard m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      reply_int(out, store.scard(m.getKeyAsStringView()));
      break;
    }

    case kSIsMember: {
      sbe::SIsMember m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      // Var-data accessors advance one shared SBE cursor. Read them in wire
      // order rather than relying on function-argument evaluation order.
      const auto key = m.getKeyAsStringView();
      const auto member = m.getMemberAsStringView();
      reply_int(out, store.sismember(key, member) ? 1 : 0);
      break;
    }

    case kSMIsMember: {
      sbe::SMIsMember m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string_view> members;
      members.clear();
      auto& g = m.members();
      while (g.hasNext()) {
        g.next();
        members.push_back(g.getMemberAsStringView());
      }
      const auto key = m.getKeyAsStringView();
      if (wrong_type(store, KeyType::Set, key, out)) break;
      static thread_local std::vector<std::string> flags;
      flags.clear();
      store.smismember(key, members, [&](bool present) {
        flags.emplace_back(present ? "1" : "0");
      });
      reply_array(out, flags);
      break;
    }

    case kSMembers: {
      sbe::SMembers m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      static thread_local std::vector<std::string> members;
      members.clear();
      store.smembers_for_each(
          m.getKeyAsStringView(), [](std::size_t) {},
          [&](EncodedStringView member) {
            members.push_back(member.to_string());
          });
      reply_array(out, members);
      break;
    }

    case kSPop: {
      sbe::SPop m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto count = m.count();
      const auto key = m.getKeyAsStringView();
      if (count == -1) {
        const auto popped = store.spop(key);
        reply_bulk_or_nil(out, popped);
        if (popped) {
          const std::string_view members[]{*popped};
          replicate_sbe_set_members(options, store, key, members);
        }
        break;
      }
      if (count < 0) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto popped = store.spop(key, static_cast<std::size_t>(count));
      reply_array(out, popped);
      static thread_local std::vector<std::string_view> members;
      members.clear();
      members.reserve(popped.size());
      for (const auto& member : popped) members.push_back(member);
      replicate_sbe_set_members(options, store, key, members);
      break;
    }

    case kSRandMember: {
      sbe::SRandMember m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto count = m.count();
      const auto key = m.getKeyAsStringView();
      if (count == -1) {
        reply_bulk_or_nil(out, store.srandmember(key));
        break;
      }
      if (count >= 0) {
        reply_array(out, store.srandmember(key, static_cast<std::size_t>(count),
                                           /*unique=*/true));
      } else {
        reply_array(out, store.srandmember(
                             key, static_cast<std::size_t>(-count),
                             /*unique=*/false));
      }
      break;
    }

    case kSMove: {
      sbe::SMove m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto source = m.getSourceAsStringView();
      const auto dest = m.getDestinationAsStringView();
      const auto member = m.getMemberAsStringView();
      if (wrong_type(store, KeyType::Set, source, out)) break;
      if (wrong_type(store, KeyType::Set, dest, out)) break;
      if (!store.value_fits(member)) {
        reply_error(out, "ERR", kValueTooLargeMsg);
        break;
      }
      const bool changed = store.smove(source, dest, member);
      reply_int(out, changed ? 1 : 0);
      if (changed) {
        const std::string_view members[]{member};
        replicate_sbe_set_members(options, store, source, members);
        replicate_sbe_set_members(options, store, dest, members);
      }
      break;
    }

    case kSInter:
    case kSUnion:
    case kSDiff: {
      static thread_local std::vector<std::string_view> keys;
      keys.clear();
      auto collect = [&](auto& msg) {
        auto& g = msg.keys();
        while (g.hasNext()) {
          g.next();
          keys.push_back(g.getKeyAsStringView());
        }
      };
      if (tid == kSInter) {
        sbe::SInter m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      } else if (tid == kSUnion) {
        sbe::SUnion m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      } else {
        sbe::SDiff m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      }
      if (keys.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for set command");
        break;
      }
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      bool bad = false;
      for (const auto key : keys) {
        if (!store.ttl_empty()) (void)store.purge_if_expired(key, now);
        if (wrong_type(store, KeyType::Set, key, out)) {
          bad = true;
          break;
        }
      }
      if (bad) break;
      const auto members =
          tid == kSInter   ? store.sinter(keys)
          : tid == kSUnion ? store.sunion(keys)
                           : store.sdiff(keys);
      reply_array(out, members);
      break;
    }

    case kSInterStore:
    case kSUnionStore:
    case kSDiffStore: {
      static thread_local std::vector<std::string_view> keys;
      keys.clear();
      std::string_view dest;
      auto collect = [&](auto& msg) {
        auto& g = msg.keys();
        while (g.hasNext()) {
          g.next();
          keys.push_back(g.getKeyAsStringView());
        }
        dest = msg.getDestinationAsStringView();
      };
      if (tid == kSInterStore) {
        sbe::SInterStore m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      } else if (tid == kSUnionStore) {
        sbe::SUnionStore m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      } else {
        sbe::SDiffStore m;
        m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
        collect(m);
      }
      if (keys.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for set command");
        break;
      }
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      bool bad = false;
      for (const auto key : keys) {
        if (!store.ttl_empty()) (void)store.purge_if_expired(key, now);
        if (wrong_type(store, KeyType::Set, key, out)) {
          bad = true;
          break;
        }
      }
      if (bad) break;
      const auto cardinality = tid == kSInterStore
                                   ? store.sinterstore(dest, keys)
                               : tid == kSUnionStore
                                   ? store.sunionstore(dest, keys)
                                   : store.sdiffstore(dest, keys);
      reply_int(out, cardinality);
      static thread_local std::vector<std::string_view> replication_args;
      replication_args.clear();
      replication_args.reserve(keys.size() + 1);
      replication_args.push_back(dest);
      replication_args.insert(replication_args.end(), keys.begin(), keys.end());
      replicate_sbe_command(
          options, store,
          tid == kSInterStore   ? CommandType::sinterstore
          : tid == kSUnionStore ? CommandType::sunionstore
                                : CommandType::sdiffstore,
          tid == kSInterStore   ? "SINTERSTORE"
          : tid == kSUnionStore ? "SUNIONSTORE"
                                : "SDIFFSTORE",
          replication_args);
      break;
    }

    case kSInterCard: {
      sbe::SInterCard m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto limit = m.limit();
      if (limit < 0) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      static thread_local std::vector<std::string_view> keys;
      keys.clear();
      auto& g = m.keys();
      while (g.hasNext()) {
        g.next();
        keys.push_back(g.getKeyAsStringView());
      }
      if (keys.empty()) {
        reply_error(out, "ERR", "wrong number of arguments for 'sintercard' command");
        break;
      }
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      bool bad = false;
      for (const auto key : keys) {
        if (!store.ttl_empty()) (void)store.purge_if_expired(key, now);
        if (wrong_type(store, KeyType::Set, key, out)) {
          bad = true;
          break;
        }
      }
      if (bad) break;
      reply_int(out, store.sintercard(keys, static_cast<std::size_t>(limit)));
      break;
    }

    case kSScan: {
      sbe::SScan m;
      m.wrapForDecode(buf, kBodyOffset, block_length, version, buflen);
      const auto cursor = m.cursor();
      const auto count = m.count();
      if (count < 1) {
        reply_error(out, "ERR", kErrNotInteger);
        break;
      }
      const auto key = m.getKeyAsStringView();
      const auto pattern = m.getMatchAsStringView();
      static thread_local std::vector<std::string> items;
      items.clear();
      // Flat reply: [next_cursor, member...].
      std::string next_cursor;
      const auto next = store.sscan(
          key, cursor, static_cast<std::size_t>(count),
          [&](std::string_view member) {
            if (pattern.empty()) return true;
            // Reuse Redis-style glob (* ? []).
            return scan_glob_match_sbe(pattern, member);
          },
          [&](std::string member) { items.push_back(std::move(member)); });
      next_cursor = std::to_string(next);
      items.insert(items.begin(), next_cursor);
      reply_array(out, items);
      break;
    }

    default:
      reply_error(out, "ERR",
                  std::string("unknown SBE template id ") + std::to_string(tid));
      break;
  }
}

}  // namespace

std::size_t sbe_dispatch_one(Store& store, std::string_view bytes, std::string& out,
                             const CommandExecutionOptions& options) {
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
    handle(store, hdr.templateId(), buf, buflen, hdr.blockLength(), hdr.version(), out, options);
  } catch (const std::exception&) {
    // Malformed / hostile frame (a bad length or var-data size trips SBE's bounds
    // check): consume it and resync, never crash.
  }
  return frame;
}

}  // namespace goblin::core
