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

#include <cstdint>
#include <cstring>
#include <exception>
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

// SBE numInGroup is uint16, so one group holds at most 65535 elements.
constexpr std::size_t kMaxGroup = 65535;

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
