#pragma once

// A C++ SBE client for the shared-memory ring. It includes the generated SBE codecs
// (header-only -- no link dependency) and speaks the length-prefixed SBE framing
// (see sbe_frame.hpp). It sends the magic once on open, then builds request frames
// and decodes the generic reply messages (Nil / Status / Int / Double / ...).
//
// This is a test/bench client: it grows one method per command as the wire does. The
// per-command shape (build request -> send_framed -> read_frame -> decode reply) is
// the client-side half of the pattern the server dispatch mirrors.

#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_frame.hpp"

#include "goblin_sbe/ArrayReply.h"
#include "goblin_sbe/DoubleReply.h"
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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core {

class SbeRingClient {
 public:
  using Scored = std::pair<double, std::string_view>;

  // Open a ring and switch it to the binary wire (send the magic once).
  [[nodiscard]] static std::optional<SbeRingClient> open(
      const char* path,
      std::chrono::milliseconds wait = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
      if (auto m = ring::Mapping::open(path)) {
        SbeRingClient c(std::move(*m));
        c.sq_.send(std::string_view(kGoblinMagicBytes, sizeof(kGoblinMagicBytes)),
                   [] { return false; });
        return c;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      ring::cpu_relax();
    }
  }

  // PING -> StatusReply "PONG".
  [[nodiscard]] bool ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::Ping p;
    p.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    send_framed(static_cast<std::uint32_t>(goblin_sbe::Ping::sbeBlockAndHeaderLength()));
    read_frame(timeout);
    if (reply_header().templateId() != goblin_sbe::StatusReply::sbeTemplateId()) {
      return false;
    }
    goblin_sbe::StatusReply r = decode<goblin_sbe::StatusReply>();
    return r.getStatusAsStringView() == "PONG";
  }

  // ZADD key [score member]... -> IntReply (native doubles, no formatting).
  [[nodiscard]] long long zadd(std::string_view key, std::span<const Scored> members,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZAdd z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.flags(0);
    auto& group = z.membersCount(static_cast<std::uint16_t>(members.size()));
    for (const auto& [score, member] : members) {
      group.next().score(score).putMember(member.data(),
                                          static_cast<std::uint32_t>(member.size()));
    }
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    send_framed(static_cast<std::uint32_t>(goblin_sbe::MessageHeader::encodedLength() +
                                           z.encodedLength()));
    read_frame(timeout);
    return decode<goblin_sbe::IntReply>().value();
  }

  // ZCARD key -> IntReply.
  [[nodiscard]] long long zcard(std::string_view key,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZCard z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    send_one(z, timeout);
    return decode<goblin_sbe::IntReply>().value();
  }

  // ZSCORE key member -> DoubleReply | NilReply.
  [[nodiscard]] std::optional<double> zscore(
      std::string_view key, std::string_view member,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZScore z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    z.putMember(member.data(), static_cast<std::uint32_t>(member.size()));
    send_one(z, timeout);
    if (reply_header().templateId() == goblin_sbe::NilReply::sbeTemplateId()) {
      return std::nullopt;
    }
    return decode<goblin_sbe::DoubleReply>().value();
  }

  // ZRANK key member -> IntReply | NilReply.
  [[nodiscard]] std::optional<long long> zrank(
      std::string_view key, std::string_view member,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZRank z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    z.putMember(member.data(), static_cast<std::uint32_t>(member.size()));
    send_one(z, timeout);
    if (reply_header().templateId() == goblin_sbe::NilReply::sbeTemplateId()) {
      return std::nullopt;
    }
    return decode<goblin_sbe::IntReply>().value();
  }

  // ZRANGE key start stop (rev=true for ZREVRANGE) -> ArrayReply members. Returns
  // owned strings (the reply frame is reused by the next call).
  [[nodiscard]] std::vector<std::string> zrange(
      std::string_view key, long long start, long long stop, bool rev = false,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZRange z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.start(start).stop(stop).withScores(0).rev(rev ? 1 : 0);
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    send_one(z, timeout);
    auto r = decode<goblin_sbe::ArrayReply>();
    std::vector<std::string> out;
    auto& g = r.items();
    while (g.hasNext()) {
      g.next();
      const auto v = g.getValueAsStringView();
      out.emplace_back(v.data(), v.size());
    }
    return out;
  }

  // ZRANGE ... WITHSCORES -> ScoredArrayReply (member, native score) pairs.
  [[nodiscard]] std::vector<std::pair<std::string, double>> zrange_withscores(
      std::string_view key, long long start, long long stop, bool rev = false,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    goblin_sbe::ZRange z;
    z.wrapAndApplyHeader(sendbuf_, kSbeLenPrefix, sizeof(sendbuf_));
    z.start(start).stop(stop).withScores(1).rev(rev ? 1 : 0);
    z.putKey(key.data(), static_cast<std::uint32_t>(key.size()));
    send_one(z, timeout);
    auto r = decode<goblin_sbe::ScoredArrayReply>();
    std::vector<std::pair<std::string, double>> out;
    auto& g = r.items();
    while (g.hasNext()) {
      g.next();
      const double score = g.score();
      const auto m = g.getMemberAsStringView();
      out.emplace_back(std::string(m.data(), m.size()), score);
    }
    return out;
  }

 private:
  explicit SbeRingClient(ring::Mapping&& m)
      : map_(std::move(m)), sq_(map_.sq_producer()), cq_(map_.cq_consumer()) {}

  // Frame a just-built message whose codec tracks its own encoded length, send it,
  // and block for the reply. (Ping uses the static-length send_framed instead.)
  template <class Msg>
  void send_one(Msg& msg, std::chrono::milliseconds timeout) {
    send_framed(static_cast<std::uint32_t>(goblin_sbe::MessageHeader::encodedLength() +
                                           msg.encodedLength()));
    read_frame(timeout);
  }

  // Backfill the 4-byte length prefix and send [len][message] in one shot.
  void send_framed(std::uint32_t msg_len) {
    std::memcpy(sendbuf_, &msg_len, kSbeLenPrefix);
    sq_.send(std::string_view(sendbuf_, kSbeLenPrefix + msg_len), [] { return false; });
  }

  [[nodiscard]] goblin_sbe::MessageHeader reply_header() {
    return goblin_sbe::MessageHeader(last_frame_.data(), last_frame_.size());
  }

  // Wrap the last reply frame for decode as `Msg` (caller already checked templateId
  // where the reply type is conditional).
  template <class Msg>
  [[nodiscard]] Msg decode() {
    const auto h = reply_header();
    Msg m;
    m.wrapForDecode(last_frame_.data(), goblin_sbe::MessageHeader::encodedLength(),
                    h.blockLength(), h.version(), last_frame_.size());
    return m;
  }

  // Block until a full length-prefixed reply frame is in cqbuf_, then copy the SBE
  // message (without the prefix) into last_frame_ for in-place decode.
  void read_frame(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      if (const auto rec = cq_.peek()) {
        cqbuf_.append(*rec);
        cq_.pop();
      }
      if (cqbuf_.size() >= kSbeLenPrefix) {
        std::uint32_t len = 0;
        std::memcpy(&len, cqbuf_.data(), kSbeLenPrefix);
        if (cqbuf_.size() >= kSbeLenPrefix + len) {
          last_frame_.assign(cqbuf_.data() + kSbeLenPrefix, len);
          cqbuf_.erase(0, kSbeLenPrefix + len);
          return;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("SbeRingClient: timed out waiting for a reply");
      }
      ring::cpu_relax();
    }
  }

  ring::Mapping map_;
  ring::Producer sq_;
  ring::Consumer cq_;
  char sendbuf_[8192];       // request scratch (SBE bounds-checks against this size)
  std::string cqbuf_;        // CQ byte accumulator
  std::string last_frame_;   // the last reply's SBE message (prefix stripped)
};

}  // namespace goblin::core
