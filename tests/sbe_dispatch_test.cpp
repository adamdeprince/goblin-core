// Drives sbe_dispatch_one directly: build a length-prefixed SBE request with the
// generated codecs, dispatch it against a Store, decode the generic SBE reply,
// assert. Proves the zero-copy no-parse zset scalar pattern (ZADD/ZCARD/ZSCORE/
// ZRANK) end to end, including native-double scores and the Nil/Double/Int/Status/
// Error reply selection.

#include "goblin/core/sbe_dispatch.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/store.hpp"

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
#include "goblin_sbe/Echo.h"
#include "goblin_sbe/GetRange.h"
#include "goblin_sbe/MGet.h"
#include "goblin_sbe/MSet.h"
#include "goblin_sbe/SetRange.h"
#include "goblin_sbe/ZRem.h"
#include "goblin_sbe/ZRemRangeByScore.h"
#include "goblin_sbe/ZRevRank.h"
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
#include "goblin_sbe/GoblinOptimize.h"
#include "goblin_sbe/Info.h"
#include "goblin_sbe/ZAdd.h"
#include "goblin_sbe/ZCard.h"
#include "goblin_sbe/ZRange.h"
#include "goblin_sbe/ZRank.h"
#include "goblin_sbe/ZScore.h"

#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace goblin::core;
namespace sbe = goblin_sbe;
using Scored = std::pair<double, std::string_view>;

namespace {

// Finish a built request: backfill the length prefix and return [len][message].
std::string framed(char* buf, std::uint32_t body_and_header) {
  std::memcpy(buf, &body_and_header, kSbeLenPrefix);
  return std::string(buf, kSbeLenPrefix + body_and_header);
}

std::string ping_frame() {
  char buf[64];
  sbe::Ping p;
  p.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  return framed(buf, static_cast<std::uint32_t>(sbe::Ping::sbeBlockAndHeaderLength()));
}

std::string zadd_frame(std::string_view key, const std::vector<Scored>& members) {
  char buf[4096];
  sbe::ZAdd z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.flags(0);
  auto& g = z.membersCount(static_cast<std::uint16_t>(members.size()));
  for (const auto& [s, m] : members) {
    g.next().score(s).putMember(m.data(), static_cast<std::uint32_t>(m.size()));
  }
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string zcard_frame(std::string_view key) {
  char buf[512];
  sbe::ZCard z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string key_member_frame(std::string_view key, std::string_view member) {
  char buf[512];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putMember(member.data(), static_cast<std::uint32_t>(member.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string zrange_frame(std::string_view key, long long start, long long stop, bool with_scores,
                         bool rev = false) {
  char buf[512];
  sbe::ZRange z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.start(start).stop(stop).withScores(with_scores ? 1 : 0).rev(rev ? 1 : 0);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string key_frame(std::string_view key) {
  char buf[512];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string key_value_frame(std::string_view key, std::string_view value) {
  char buf[4096];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string idelta_key_frame(long long delta, std::string_view key) {
  char buf[512];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.delta(delta);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string incrbyfloat_frame(double delta, std::string_view key) {
  char buf[512];
  sbe::IncrByFloat z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.delta(delta);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string set_frame(std::string_view key, std::string_view value, std::uint8_t flags = 0,
                      std::uint8_t mode = 0, long long expire_value = 0) {
  char buf[4096];
  sbe::Set z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.flags(flags).expireMode(mode).expireValue(expire_value);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string keys_frame(const std::vector<std::string_view>& keys) {
  char buf[4096];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  auto& g = z.keysCount(static_cast<std::uint16_t>(keys.size()));
  for (const auto& k : keys) {
    g.next().putKey(k.data(), static_cast<std::uint32_t>(k.size()));
  }
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string expire_frame(long long amount, std::uint8_t flags, std::string_view key) {
  char buf[512];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.amount(amount).flags(flags);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string hset_frame(std::string_view key,
                       const std::vector<std::pair<std::string_view, std::string_view>>& pairs) {
  char buf[4096];
  sbe::HSet z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  auto& g = z.entriesCount(static_cast<std::uint16_t>(pairs.size()));
  for (const auto& [f, v] : pairs) {
    g.next().putField(f.data(), static_cast<std::uint32_t>(f.size()))
            .putValue(v.data(), static_cast<std::uint32_t>(v.size()));
  }
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string hsetnx_frame(std::string_view key, std::string_view field, std::string_view value) {
  char buf[4096];
  sbe::HSetNx z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putField(field.data(), static_cast<std::uint32_t>(field.size()));
  z.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string key_field_frame(std::string_view key, std::string_view field) {
  char buf[512];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putField(field.data(), static_cast<std::uint32_t>(field.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string hfields_frame(std::string_view key, const std::vector<std::string_view>& fields) {
  char buf[4096];
  Msg z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  auto& g = z.fieldsCount(static_cast<std::uint16_t>(fields.size()));
  for (const auto& f : fields) {
    g.next().putField(f.data(), static_cast<std::uint32_t>(f.size()));
  }
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string hincrby_frame(std::string_view key, std::string_view field, long long delta) {
  char buf[512];
  sbe::HIncrBy z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.delta(delta);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putField(field.data(), static_cast<std::uint32_t>(field.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

// HSET with a single oversized value (heap buffer), for the 64 KiB cap.
std::string big_hset_frame(std::string_view key, std::string_view field, const std::string& value) {
  std::vector<char> buf(value.size() + 256);
  sbe::HSet z;
  z.wrapAndApplyHeader(buf.data(), kSbeLenPrefix, buf.size());
  z.entriesCount(1).next().putField(field.data(), static_cast<std::uint32_t>(field.size()))
      .putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf.data(), static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string zrem_frame(std::string_view key, const std::vector<std::string_view>& members) {
  char buf[4096];
  sbe::ZRem z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  auto& g = z.membersCount(static_cast<std::uint16_t>(members.size()));
  for (const auto& m : members) g.next().putMember(m.data(), static_cast<std::uint32_t>(m.size()));
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string zremrangebyscore_frame(std::string_view key, double min, bool min_excl, double max,
                                   bool max_excl) {
  char buf[512];
  sbe::ZRemRangeByScore z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.min(min).minExclusive(min_excl ? 1 : 0).max(max).maxExclusive(max_excl ? 1 : 0);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string getrange_frame(std::string_view key, long long start, long long end) {
  char buf[512];
  sbe::GetRange z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.start(start).end(end);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string setrange_frame(std::string_view key, long long byte_offset, std::string_view value) {
  char buf[4096];
  sbe::SetRange z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.byteOffset(byte_offset);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  z.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string mset_frame(const std::vector<std::pair<std::string_view, std::string_view>>& pairs) {
  char buf[4096];
  sbe::MSet z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  auto& g = z.pairsCount(static_cast<std::uint16_t>(pairs.size()));
  for (const auto& [k, v] : pairs) {
    g.next().putKey(k.data(), static_cast<std::uint32_t>(k.size()))
            .putValue(v.data(), static_cast<std::uint32_t>(v.size()));
  }
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

std::string echo_frame(std::string_view message) {
  char buf[4096];
  sbe::Echo z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putMessage(message.data(), static_cast<std::uint32_t>(message.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}

template <class Msg>
std::string fin(char* buf, Msg& z) {  // finish: length prefix from the codec's length
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
}
auto sv32 = [](std::string_view s) { return static_cast<std::uint32_t>(s.size()); };

std::string cad_frame(std::string_view key, std::string_view token) {
  char buf[512]; sbe::GoblinCad z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), sv32(key)); z.putToken(token.data(), sv32(token));
  return fin(buf, z);
}
std::string caexpire_frame(std::string_view key, std::string_view token, long long ms) {
  char buf[512]; sbe::GoblinCaExpire z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.ms(ms); z.putKey(key.data(), sv32(key)); z.putToken(token.data(), sv32(token));
  return fin(buf, z);
}
std::string cas_frame(std::string_view key, std::string_view token, std::string_view value) {
  char buf[4096]; sbe::GoblinCas z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), sv32(key)); z.putToken(token.data(), sv32(token)); z.putValue(value.data(), sv32(value));
  return fin(buf, z);
}
std::string tdrescore_frame(std::string_view key, double now, double hl, long long k, std::uint8_t mode) {
  char buf[512]; sbe::GoblinTdRescore z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.now(now).hl(hl).k(k).mode(mode); z.putKey(key.data(), sv32(key));
  return fin(buf, z);
}
std::string increx_frame(std::string_view key, long long seconds) {
  char buf[512]; sbe::GoblinIncrEx z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.seconds(seconds); z.putKey(key.data(), sv32(key));
  return fin(buf, z);
}
std::string zwindow_frame(std::string_view key, double now, double window, long long limit, std::string_view member) {
  char buf[512]; sbe::GoblinZWindow z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.now(now).window(window).limit(limit); z.putKey(key.data(), sv32(key)); z.putMember(member.data(), sv32(member));
  return fin(buf, z);
}
std::string incrbound_frame(std::string_view key, long long delta, long long max) {
  char buf[512]; sbe::GoblinIncrBound z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.delta(delta).max(max); z.putKey(key.data(), sv32(key));
  return fin(buf, z);
}
std::string hcad_frame(std::string_view key, std::string_view field, std::string_view expected) {
  char buf[512]; sbe::GoblinHCad z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), sv32(key)); z.putField(field.data(), sv32(field)); z.putExpected(expected.data(), sv32(expected));
  return fin(buf, z);
}
std::string hsetgt_frame(std::string_view key, std::string_view field, std::string_view value) {
  char buf[512]; sbe::GoblinHSetGt z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.putKey(key.data(), sv32(key)); z.putField(field.data(), sv32(field)); z.putValue(value.data(), sv32(value));
  return fin(buf, z);
}
std::string claim_frame(std::string_view ck, std::string_view rk, std::string_view token, long long seconds) {
  char buf[512]; sbe::GoblinClaim z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.seconds(seconds); z.putClaimKey(ck.data(), sv32(ck)); z.putResultKey(rk.data(), sv32(rk)); z.putToken(token.data(), sv32(token));
  return fin(buf, z);
}
std::string optimize_frame(std::string_view key, double density) {
  char buf[512]; sbe::GoblinOptimize z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.density(density); z.putKey(key.data(), sv32(key));
  return fin(buf, z);
}
std::string info_frame() {
  char buf[64]; sbe::Info z; z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  return framed(buf, static_cast<std::uint32_t>(sbe::Info::sbeBlockAndHeaderLength()));
}

// Dispatch a frame, assert it is fully consumed, hand the reply header + message to
// `check`.
template <class Check>
void dispatch(Store& store, const std::string& frame, Check&& check) {
  std::string out;
  const std::size_t consumed = sbe_dispatch_one(store, frame, out);
  assert(consumed == frame.size());
  assert(!out.empty());
  std::uint32_t len = 0;
  std::memcpy(&len, out.data(), kSbeLenPrefix);
  assert(out.size() == kSbeLenPrefix + len);
  char* msg = out.data() + kSbeLenPrefix;
  sbe::MessageHeader hdr(msg, len);
  check(hdr, msg, len);
}

template <class Msg>
Msg decode(sbe::MessageHeader& hdr, char* msg, std::uint32_t len) {
  assert(hdr.templateId() == Msg::sbeTemplateId());
  Msg m;
  m.wrapForDecode(msg, sbe::MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);
  return m;
}

}  // namespace

int main() {
  Store store;

  // PING -> StatusReply "PONG"
  dispatch(store, ping_frame(), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::StatusReply>(h, m, l).getStatusAsStringView() == "PONG");
  });

  // ZADD board 1.5 alice 2.0 bob -> 2 (native doubles, members-then-key decode)
  dispatch(store, zadd_frame("board", {{1.5, "alice"}, {2.0, "bob"}}),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::IntReply>(h, m, l).value() == 2); });
  dispatch(store, zadd_frame("board", {{3.0, "carol"}}),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::IntReply>(h, m, l).value() == 1); });

  // ZCARD board -> 3
  dispatch(store, zcard_frame("board"),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::IntReply>(h, m, l).value() == 3); });
  // ZCARD missing -> 0
  dispatch(store, zcard_frame("nope"),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::IntReply>(h, m, l).value() == 0); });

  // ZSCORE board bob -> 2.0 (native double back, no restringify)
  dispatch(store, key_member_frame<sbe::ZScore>("board", "bob"),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::DoubleReply>(h, m, l).value() == 2.0); });
  // ZSCORE board ghost -> Nil
  dispatch(store, key_member_frame<sbe::ZScore>("board", "ghost"),
           [](sbe::MessageHeader& h, char*, std::uint32_t) { assert(h.templateId() == sbe::NilReply::sbeTemplateId()); });

  // ZRANK board alice -> 0 (lowest score) ; ZRANK board ghost -> Nil
  dispatch(store, key_member_frame<sbe::ZRank>("board", "alice"),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) { assert(decode<sbe::IntReply>(h, m, l).value() == 0); });
  dispatch(store, key_member_frame<sbe::ZRank>("board", "ghost"),
           [](sbe::MessageHeader& h, char*, std::uint32_t) { assert(h.templateId() == sbe::NilReply::sbeTemplateId()); });

  // ZRANGE board 0 -1 -> ArrayReply [alice, bob, carol] (score order 1.5/2.0/3.0)
  dispatch(store, zrange_frame("board", 0, -1, false), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    auto r = decode<sbe::ArrayReply>(h, m, l);
    std::vector<std::string> got;
    auto& g = r.items();
    while (g.hasNext()) {
      g.next();
      const auto v = g.getValueAsStringView();
      got.emplace_back(v.data(), v.size());
    }
    assert((got == std::vector<std::string>{"alice", "bob", "carol"}));
  });
  // ZRANGE board 0 -1 WITHSCORES -> ScoredArrayReply (native doubles)
  dispatch(store, zrange_frame("board", 0, -1, true), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    auto r = decode<sbe::ScoredArrayReply>(h, m, l);
    std::vector<std::pair<std::string, double>> got;
    auto& g = r.items();
    while (g.hasNext()) {
      g.next();
      const double s = g.score();
      const auto mm = g.getMemberAsStringView();
      got.emplace_back(std::string(mm.data(), mm.size()), s);
    }
    assert(got.size() == 3);
    assert(got[0].first == "alice" && got[0].second == 1.5);
    assert(got[2].first == "carol" && got[2].second == 3.0);
  });
  // Empty range -> empty ArrayReply (no elements, still a valid array not a Nil)
  dispatch(store, zrange_frame("nope", 0, -1, false), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    auto r = decode<sbe::ArrayReply>(h, m, l);
    assert(!r.items().hasNext());
  });

  // ---- String core (batch 1) ----
  auto is_nil = [](sbe::MessageHeader& h, char*, std::uint32_t) {
    assert(h.templateId() == sbe::NilReply::sbeTemplateId());
  };
  auto bulk_is = [](std::string_view want) {
    return [want](sbe::MessageHeader& h, char* m, std::uint32_t l) {
      assert(decode<sbe::BulkReply>(h, m, l).getValueAsStringView() == want);
    };
  };
  auto int_is = [](long long want) {
    return [want](sbe::MessageHeader& h, char* m, std::uint32_t l) {
      assert(decode<sbe::IntReply>(h, m, l).value() == want);
    };
  };

  // SET/GET, Nil on miss
  dispatch(store, set_frame("s", "hello"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::StatusReply>(h, m, l).getStatusAsStringView() == "OK");
  });
  dispatch(store, key_frame<sbe::Get>("s"), bulk_is("hello"));
  dispatch(store, key_frame<sbe::Get>("absent"), is_nil);
  // STRLEN, APPEND
  dispatch(store, key_frame<sbe::StrLen>("s"), int_is(5));
  dispatch(store, key_value_frame<sbe::Append>("s", " world"), int_is(11));
  dispatch(store, key_frame<sbe::Get>("s"), bulk_is("hello world"));
  // GETSET returns old, GETDEL returns then removes
  dispatch(store, key_value_frame<sbe::GetSet>("s", "new"), bulk_is("hello world"));
  dispatch(store, key_frame<sbe::GetDel>("s"), bulk_is("new"));
  dispatch(store, key_frame<sbe::Get>("s"), is_nil);
  // SETNX: sets once, then no-op
  dispatch(store, key_value_frame<sbe::SetNx>("fresh", "v"), int_is(1));
  dispatch(store, key_value_frame<sbe::SetNx>("fresh", "v2"), int_is(0));
  // INCR / DECR / INCRBY / DECRBY
  dispatch(store, key_frame<sbe::Incr>("cnt"), int_is(1));
  dispatch(store, key_frame<sbe::Incr>("cnt"), int_is(2));
  dispatch(store, idelta_key_frame<sbe::IncrBy>(10, "cnt"), int_is(12));
  dispatch(store, key_frame<sbe::Decr>("cnt"), int_is(11));
  dispatch(store, idelta_key_frame<sbe::DecrBy>(5, "cnt"), int_is(6));
  // INCR on a non-integer -> ErrorReply ERR
  dispatch(store, set_frame("word", "abc"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, key_frame<sbe::Incr>("word"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::ErrorReply>(h, m, l).getCodeAsStringView() == "ERR");
  });
  // INCRBYFLOAT (native double in, formatted bulk out) -- compare numerically
  dispatch(store, incrbyfloat_frame(2.5, "f"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(std::stod(std::string(decode<sbe::BulkReply>(h, m, l).getValueAsStringView())) == 2.5);
  });
  dispatch(store, incrbyfloat_frame(0.5, "f"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(std::stod(std::string(decode<sbe::BulkReply>(h, m, l).getValueAsStringView())) == 3.0);
  });
  // SET NX on an existing key and SET XX on a missing key both no-op -> Nil
  dispatch(store, set_frame("fresh", "x", 0x01), is_nil);       // NX, exists
  dispatch(store, set_frame("ghostkey", "x", 0x02), is_nil);    // XX, missing
  // SET ... GET returns the prior value ("fresh" still holds "v") and overwrites
  dispatch(store, set_frame("fresh", "z", 0x04), bulk_is("v"));
  dispatch(store, key_frame<sbe::Get>("fresh"), bulk_is("z"));

  // ---- Keyspace / TTL (batch 2) ----
  auto status_is = [](std::string_view want) {
    return [want](sbe::MessageHeader& h, char* m, std::uint32_t l) {
      assert(decode<sbe::StatusReply>(h, m, l).getStatusAsStringView() == want);
    };
  };
  dispatch(store, set_frame("ka", "1"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, set_frame("kb", "2"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  // EXISTS counts present keys; TYPE reports the kind
  dispatch(store, keys_frame<sbe::Exists>({"ka", "kb", "missing"}), int_is(2));
  dispatch(store, key_frame<sbe::Type>("ka"), status_is("string"));
  dispatch(store, key_frame<sbe::Type>("board"), status_is("zset"));
  dispatch(store, key_frame<sbe::Type>("missing"), status_is("none"));
  // EXPIRE sets a ttl; TTL/PTTL/EXPIRETIME read it; PERSIST clears it
  dispatch(store, expire_frame<sbe::Expire>(1000, 0, "ka"), int_is(1));
  dispatch(store, key_frame<sbe::Ttl>("ka"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    const auto t = decode<sbe::IntReply>(h, m, l).value();
    assert(t > 0 && t <= 1000);
  });
  dispatch(store, key_frame<sbe::PTtl>("ka"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::IntReply>(h, m, l).value() > 0);
  });
  dispatch(store, key_frame<sbe::ExpireTime>("ka"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::IntReply>(h, m, l).value() > 0);  // a future unix time
  });
  dispatch(store, key_frame<sbe::Persist>("ka"), int_is(1));
  dispatch(store, key_frame<sbe::Ttl>("ka"), int_is(-1));       // exists, no ttl
  dispatch(store, key_frame<sbe::Ttl>("missing"), int_is(-2));  // no key
  // EXPIRE flags: GT rejects a smaller expiry, GT+LT is incompatible
  dispatch(store, expire_frame<sbe::Expire>(1000, 0, "kb"), int_is(1));
  dispatch(store, expire_frame<sbe::Expire>(10, 0x04, "kb"), int_is(0));  // GT and 10 < 1000
  dispatch(store, expire_frame<sbe::Expire>(5, 0x0C, "kb"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::ErrorReply>(h, m, l).getCodeAsStringView() == "ERR");  // GT | LT
  });
  // All four expire variants apply (PEXPIRE ms-relative, *AT absolute)
  dispatch(store, expire_frame<sbe::PExpire>(500000, 0, "ka"), int_is(1));
  dispatch(store, expire_frame<sbe::ExpireAt>(4102444800LL, 0, "ka"), int_is(1));      // ~year 2100 s
  dispatch(store, expire_frame<sbe::PExpireAt>(4102444800000LL, 0, "kb"), int_is(1));  // ~year 2100 ms
  dispatch(store, key_frame<sbe::PExpireTime>("kb"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::IntReply>(h, m, l).value() > 0);
  });
  // DEL removes; EXISTS then sees nothing
  dispatch(store, keys_frame<sbe::Del>({"ka", "kb"}), int_is(2));
  dispatch(store, keys_frame<sbe::Exists>({"ka", "kb"}), int_is(0));

  // ---- Hash (batch 3) ----
  auto count_items = [](sbe::MessageHeader& h, char* m, std::uint32_t l) -> long long {
    auto r = decode<sbe::ArrayReply>(h, m, l);
    long long n = 0;
    auto& g = r.items();
    while (g.hasNext()) { g.next(); g.getValueAsStringView(); ++n; }
    return n;
  };
  // HSET two fields; HGET, HLEN, HEXISTS, HSTRLEN
  dispatch(store, hset_frame("h", {{"f1", "v1"}, {"f2", "v2"}}), int_is(2));
  dispatch(store, key_field_frame<sbe::HGet>("h", "f1"), bulk_is("v1"));
  dispatch(store, key_field_frame<sbe::HGet>("h", "nope"), is_nil);
  dispatch(store, key_frame<sbe::HLen>("h"), int_is(2));
  dispatch(store, key_field_frame<sbe::HExists>("h", "f2"), int_is(1));
  dispatch(store, key_field_frame<sbe::HExists>("h", "nope"), int_is(0));
  dispatch(store, key_field_frame<sbe::HStrLen>("h", "f1"), int_is(2));  // "v1"
  // HSETNX sets once
  dispatch(store, hsetnx_frame("h", "f3", "v3"), int_is(1));
  dispatch(store, hsetnx_frame("h", "f3", "x"), int_is(0));
  // HMGET -> nullable array [v1, nil, v3]
  dispatch(store, hfields_frame<sbe::HMGet>("h", {"f1", "nope", "f3"}),
           [](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
             auto r = decode<sbe::NullableArrayReply>(hd, m, l);
             std::vector<std::optional<std::string>> got;
             auto& g = r.items();
             while (g.hasNext()) {
               g.next();
               const bool present = g.present() != 0;
               const auto v = g.getValueAsStringView();  // always advance
               if (present) got.emplace_back(std::string(v.data(), v.size()));
               else got.emplace_back(std::nullopt);
             }
             assert(got.size() == 3);
             assert(got[0] == "v1" && !got[1].has_value() && got[2] == "v3");
           });
  // HGETALL -> flat [field value]... (order-independent check)
  dispatch(store, key_frame<sbe::HGetAll>("h"), [](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
    auto r = decode<sbe::ArrayReply>(hd, m, l);
    std::vector<std::string> flat;
    auto& g = r.items();
    while (g.hasNext()) { g.next(); const auto v = g.getValueAsStringView(); flat.emplace_back(v.data(), v.size()); }
    assert(flat.size() == 6);
    std::map<std::string, std::string> hv;
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) hv[flat[i]] = flat[i + 1];
    assert(hv["f1"] == "v1" && hv["f2"] == "v2" && hv["f3"] == "v3");
  });
  // HKEYS / HVALS -> 3 elements each
  dispatch(store, key_frame<sbe::HKeys>("h"), [&](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
    assert(count_items(hd, m, l) == 3);
  });
  dispatch(store, key_frame<sbe::HVals>("h"), [&](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
    assert(count_items(hd, m, l) == 3);
  });
  // HINCRBY accumulates; on a non-integer field -> ErrorReply
  dispatch(store, hincrby_frame("hc", "cnt", 5), int_is(5));
  dispatch(store, hincrby_frame("hc", "cnt", 3), int_is(8));
  dispatch(store, hset_frame("hc", {{"txt", "abc"}}), int_is(1));
  dispatch(store, hincrby_frame("hc", "txt", 1), [](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
    assert(decode<sbe::ErrorReply>(hd, m, l).getCodeAsStringView() == "ERR");
  });
  // A hash value over the 64 KiB cap -> ERR (existing clients see the error + reason)
  {
    const std::string big(70000, 'x');
    dispatch(store, big_hset_frame("hbig", "f", big), [](sbe::MessageHeader& hd, char* m, std::uint32_t l) {
      assert(decode<sbe::ErrorReply>(hd, m, l).getCodeAsStringView() == "ERR");
    });
  }
  // HDEL removes named fields
  dispatch(store, hfields_frame<sbe::HDel>("h", {"f1", "f2", "nope"}), int_is(2));
  dispatch(store, key_frame<sbe::HLen>("h"), int_is(1));  // f3 remains

  // ---- Standard tail (batch 4) ----
  dispatch(store, zadd_frame("lb", {{1.0, "a"}, {2.0, "b"}, {3.0, "c"}}), int_is(3));
  // ZREVRANK: the highest score is rank 0
  dispatch(store, key_member_frame<sbe::ZRevRank>("lb", "c"), int_is(0));
  dispatch(store, key_member_frame<sbe::ZRevRank>("lb", "a"), int_is(2));
  dispatch(store, key_member_frame<sbe::ZRevRank>("lb", "ghost"), is_nil);
  // ZREVRANGE via ZRange rev=1 -> reverse order [c, b, a]
  dispatch(store, zrange_frame("lb", 0, -1, false, true), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    auto r = decode<sbe::ArrayReply>(h, m, l);
    std::vector<std::string> got;
    auto& g = r.items();
    while (g.hasNext()) { g.next(); const auto v = g.getValueAsStringView(); got.emplace_back(v.data(), v.size()); }
    assert((got == std::vector<std::string>{"c", "b", "a"}));
  });
  // ZREM
  dispatch(store, zrem_frame("lb", {"a", "b"}), int_is(2));
  dispatch(store, key_frame<sbe::ZCard>("lb"), int_is(1));  // c remains
  // ZREMRANGEBYSCORE [2,3] inclusive removes b and c
  dispatch(store, zadd_frame("z2", {{1.0, "a"}, {2.0, "b"}, {3.0, "c"}, {4.0, "d"}}), int_is(4));
  dispatch(store, zremrangebyscore_frame("z2", 2.0, false, 3.0, false), int_is(2));
  dispatch(store, key_frame<sbe::ZCard>("z2"), int_is(2));  // a, d remain
  // GETRANGE / SETRANGE
  dispatch(store, set_frame("gr", "Hello World"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, getrange_frame("gr", 0, 4), bulk_is("Hello"));
  dispatch(store, setrange_frame("gr", 6, "Redis"), int_is(11));
  dispatch(store, key_frame<sbe::Get>("gr"), bulk_is("Hello Redis"));
  // MSET / MGET (nullable array)
  dispatch(store, mset_frame({{"m1", "v1"}, {"m2", "v2"}}), status_is("OK"));
  dispatch(store, key_frame<sbe::Get>("m1"), bulk_is("v1"));
  dispatch(store, keys_frame<sbe::MGet>({"m1", "m2", "missing"}),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
             auto r = decode<sbe::NullableArrayReply>(h, m, l);
             std::vector<std::optional<std::string>> got;
             auto& g = r.items();
             while (g.hasNext()) {
               g.next();
               const bool present = g.present() != 0;
               const auto v = g.getValueAsStringView();
               if (present) got.emplace_back(std::string(v.data(), v.size()));
               else got.emplace_back(std::nullopt);
             }
             assert(got.size() == 3 && got[0] == "v1" && got[1] == "v2" && !got[2].has_value());
           });
  // ECHO
  dispatch(store, echo_frame("hi there"), bulk_is("hi there"));

  // ---- GOBLIN.* natives (batch 5) ----
  // CAD: delete only on a token match
  dispatch(store, set_frame("lock", "tok1"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, cad_frame("lock", "wrong"), int_is(0));
  dispatch(store, cad_frame("lock", "tok1"), int_is(1));
  dispatch(store, key_frame<sbe::Get>("lock"), is_nil);
  // CAEXPIRE: renew TTL only on a token match
  dispatch(store, set_frame("lk", "t"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, caexpire_frame("lk", "t", 5000), int_is(1));
  dispatch(store, caexpire_frame("lk", "wrong", 5000), int_is(0));
  // CAS: OK on swap (KEEPTTL), 0 on mismatch
  dispatch(store, set_frame("cx", "v1"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, cas_frame("cx", "v1", "v2"), status_is("OK"));
  dispatch(store, key_frame<sbe::Get>("cx"), bulk_is("v2"));
  dispatch(store, cas_frame("cx", "wrong", "v3"), int_is(0));
  // TD_LEADERBOARD_RESCORE LINEAR: native weights, top-2 [a:1.0, b:0.5]
  dispatch(store, zadd_frame("ldr", {{100.0, "a"}, {90.0, "b"}, {80.0, "c"}}), int_is(3));
  dispatch(store, tdrescore_frame("ldr", 100.0, 10.0, 2, 0),
           [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
             auto r = decode<sbe::ScoredArrayReply>(h, m, l);
             std::vector<std::pair<std::string, double>> got;
             auto& g = r.items();
             while (g.hasNext()) {
               g.next();
               const double s = g.score();
               const auto mm = g.getMemberAsStringView();
               got.emplace_back(std::string(mm.data(), mm.size()), s);
             }
             assert(got.size() == 2);
             assert(got[0].first == "a" && got[0].second == 1.0);
             assert(got[1].first == "b" && got[1].second == 0.5);
           });
  // INCREX: increments and arms a TTL on first write
  dispatch(store, increx_frame("rl", 60), int_is(1));
  dispatch(store, increx_frame("rl", 60), int_is(2));
  dispatch(store, key_frame<sbe::Ttl>("rl"), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::IntReply>(h, m, l).value() > 0);
  });
  // ZWINDOW: admit up to the limit within the window
  dispatch(store, zwindow_frame("win", 100.0, 10.0, 2, "r1"), int_is(1));
  dispatch(store, zwindow_frame("win", 100.0, 10.0, 2, "r2"), int_is(1));
  dispatch(store, zwindow_frame("win", 100.0, 10.0, 2, "r3"), int_is(0));  // full
  // INCRBOUND: admit if the result stays <= max, else -1
  dispatch(store, set_frame("q", "5"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, incrbound_frame("q", 3, 10), int_is(8));
  dispatch(store, incrbound_frame("q", 5, 10), int_is(-1));  // 8 + 5 > 10
  // DECRPOS: reserve while > 0, else -1
  dispatch(store, set_frame("stock", "2"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, key_frame<sbe::GoblinDecrPos>("stock"), int_is(1));
  dispatch(store, key_frame<sbe::GoblinDecrPos>("stock"), int_is(0));
  dispatch(store, key_frame<sbe::GoblinDecrPos>("stock"), int_is(-1));
  // HCAD: delete a hash field only on match
  dispatch(store, hset_frame("hl2", {{"f", "tok"}}), int_is(1));
  dispatch(store, hcad_frame("hl2", "f", "wrong"), int_is(0));
  dispatch(store, hcad_frame("hl2", "f", "tok"), int_is(1));
  // HSETGT: set only if greater (value stored as the exact string)
  dispatch(store, hset_frame("hw", {{"ts", "100"}}), int_is(1));
  dispatch(store, hsetgt_frame("hw", "ts", "90"), int_is(0));
  dispatch(store, hsetgt_frame("hw", "ts", "150"), int_is(1));
  dispatch(store, key_field_frame<sbe::HGet>("hw", "ts"), bulk_is("150"));
  // CLAIM: first wins (CLAIMED); later reads the result slot
  dispatch(store, claim_frame("ck", "rk", "tok1", 60), bulk_is("CLAIMED"));
  dispatch(store, claim_frame("ck", "rk", "tok2", 60), is_nil);  // held, result empty
  dispatch(store, set_frame("rk", "done"), [](sbe::MessageHeader&, char*, std::uint32_t) {});
  dispatch(store, claim_frame("ck", "rk", "tok3", 60), bulk_is("done"));

  // ---- Admin (batch 6) ----
  // OPTIMIZE a zset -> IntReply; missing key -> Nil; density > 1 -> Error
  dispatch(store, zadd_frame("opt", {{1.0, "a"}, {2.0, "b"}}), int_is(2));
  dispatch(store, optimize_frame("opt", 0.0), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::IntReply>(h, m, l).value() >= 0);  // default density
  });
  dispatch(store, optimize_frame("missingkey", 0.0), is_nil);
  dispatch(store, optimize_frame("opt", 1.5), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    assert(decode<sbe::ErrorReply>(h, m, l).getCodeAsStringView() == "ERR");
  });
  // INFO -> a non-empty bulk with known fields
  dispatch(store, info_frame(), [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
    const auto s = decode<sbe::BulkReply>(h, m, l).getValueAsStringView();
    assert(!s.empty() && s.find("used_memory") != std::string_view::npos);
  });

  // Unknown templateId -> ErrorReply (not a crash): patch a ZCARD frame's id.
  {
    std::string f = zcard_frame("k");
    const std::uint16_t bogus = 9999;
    std::memcpy(f.data() + kSbeLenPrefix + 2, &bogus, sizeof(bogus));  // templateId slot
    dispatch(store, f, [](sbe::MessageHeader& h, char* m, std::uint32_t l) {
      auto e = decode<sbe::ErrorReply>(h, m, l);
      assert(e.getCodeAsStringView() == "ERR");
    });
  }

  // Two frames concatenated: exactly one consumed, the rest left for the next call.
  {
    const std::string a = zcard_frame("board");
    const std::string b = ping_frame();
    std::string out;
    assert(sbe_dispatch_one(store, a + b, out) == a.size());
  }

  std::puts("sbe dispatch OK: + admin OPTIMIZE/INFO (63 commands) -> Store (no parse) -> generic reply");
  return 0;
}
