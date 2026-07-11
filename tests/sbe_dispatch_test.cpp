// Drives sbe_dispatch_one directly: build a length-prefixed SBE request with the
// generated codecs, dispatch it against a Store, decode the generic SBE reply,
// assert. Proves the zero-copy no-parse zset scalar pattern (ZADD/ZCARD/ZSCORE/
// ZRANK) end to end, including native-double scores and the Nil/Double/Int/Status/
// Error reply selection.

#include "goblin/core/sbe_dispatch.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/store.hpp"

#include "goblin_sbe/ArrayReply.h"
#include "goblin_sbe/DoubleReply.h"
#include "goblin_sbe/ErrorReply.h"
#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/NilReply.h"
#include "goblin_sbe/Ping.h"
#include "goblin_sbe/ScoredArrayReply.h"
#include "goblin_sbe/StatusReply.h"
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

std::string zrange_frame(std::string_view key, long long start, long long stop, bool with_scores) {
  char buf[512];
  sbe::ZRange z;
  z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
  z.start(start).stop(stop).withScores(with_scores ? 1 : 0).rev(0);
  z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
  return framed(buf, static_cast<std::uint32_t>(sbe::MessageHeader::encodedLength() + z.encodedLength()));
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

  std::puts("sbe dispatch OK: zset scalar + array patterns -> Store (no parse) -> generic reply");
  return 0;
}
