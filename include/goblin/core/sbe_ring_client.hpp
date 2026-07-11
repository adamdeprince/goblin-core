#pragma once

// A C++ SBE client for the shared-memory ring, covering the full command surface.
// It includes the generated SBE codecs (header-only -- no link dependency) and speaks
// the length-prefixed SBE framing (see sbe_frame.hpp): it sends the GOBLINS! magic
// once on open, then for each call builds a typed request, sends it, and decodes the
// typed reply. An ErrorReply becomes a thrown std::runtime_error ("<code> <message>").
//
// Every request builds into a growable send buffer (values may be up to 64 KiB), and
// replies are decoded straight out of the last received frame.

#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_frame.hpp"

#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/Ping.h"
// Replies
#include "goblin_sbe/ArrayReply.h"
#include "goblin_sbe/BulkReply.h"
#include "goblin_sbe/DoubleReply.h"
#include "goblin_sbe/ErrorReply.h"
#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MapReply.h"
#include "goblin_sbe/NilReply.h"
#include "goblin_sbe/NullableArrayReply.h"
#include "goblin_sbe/RespValueReply.h"
#include "goblin_sbe/ScoredArrayReply.h"
#include "goblin_sbe/StatusReply.h"
// String
#include "goblin_sbe/Append.h"
#include "goblin_sbe/Decr.h"
#include "goblin_sbe/DecrBy.h"
#include "goblin_sbe/Get.h"
#include "goblin_sbe/GetDel.h"
#include "goblin_sbe/GetRange.h"
#include "goblin_sbe/GetSet.h"
#include "goblin_sbe/Incr.h"
#include "goblin_sbe/IncrBy.h"
#include "goblin_sbe/IncrByFloat.h"
#include "goblin_sbe/MGet.h"
#include "goblin_sbe/MSet.h"
#include "goblin_sbe/Set.h"
#include "goblin_sbe/SetNx.h"
#include "goblin_sbe/SetRange.h"
#include "goblin_sbe/StrLen.h"
// Keyspace / TTL
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
// Hash
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
// Zset
#include "goblin_sbe/ZAdd.h"
#include "goblin_sbe/ZCard.h"
#include "goblin_sbe/ZRange.h"
#include "goblin_sbe/ZRank.h"
#include "goblin_sbe/ZRem.h"
#include "goblin_sbe/ZRemRangeByScore.h"
#include "goblin_sbe/ZRevRank.h"
#include "goblin_sbe/ZScore.h"
// GOBLIN.* natives + admin
#include "goblin_sbe/Echo.h"
#include "goblin_sbe/GoblinCaExpire.h"
#include "goblin_sbe/GoblinCad.h"
#include "goblin_sbe/GoblinCas.h"
#include "goblin_sbe/GoblinClaim.h"
#include "goblin_sbe/GoblinDecrPos.h"
#include "goblin_sbe/GoblinHCad.h"
#include "goblin_sbe/GoblinHSetGt.h"
#include "goblin_sbe/GoblinIncrBound.h"
#include "goblin_sbe/GoblinIncrEx.h"
#include "goblin_sbe/GoblinLoad.h"
#include "goblin_sbe/GoblinMemory.h"
#include "goblin_sbe/GoblinOptimize.h"
#include "goblin_sbe/GoblinSave.h"
#include "goblin_sbe/GoblinTdRescore.h"
#include "goblin_sbe/GoblinZWindow.h"
#include "goblin_sbe/Info.h"
// Scripting
#include "goblin_sbe/Eval.h"
#include "goblin_sbe/EvalSha.h"
#include "goblin_sbe/Script.h"

#include <algorithm>
#include <any>
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

// The embedded language for a script command (matches the wire's language byte).
enum class Language : std::uint8_t { lua = 0, luau, wren, tcl, micropython, quickjs };

// SET options; expireMode: 0 none, 1 EX, 2 PX, 3 EXAT, 4 PXAT.
struct SetOptions {
  bool nx = false, xx = false, get = false, keepttl = false;
  std::uint8_t expire_mode = 0;
  long long expire_value = 0;
};
// SET's reply: `ok` on +OK, `old` when GET returned a prior value; both empty on nil.
struct SetReply {
  bool ok = false;
  std::optional<std::string> old;
};

// A decoded script reply (the flattened RespValueReply rebuilt into a tree).
struct RespValue {
  enum class Type : std::uint8_t { nil, integer, bulk, status, error, array };
  Type type = Type::nil;
  long long integer = 0;
  std::string str;                  // bulk / status / error text
  std::vector<RespValue> elements;  // array (map: flattened key,value,key,value...)
};

class SbeRingClient {
 public:
  using Scored = std::pair<double, std::string_view>;
  using ms = std::chrono::milliseconds;
  static constexpr ms kDefaultTimeout{5000};
  // Initial size of the request/reply buffers. It is a floor: the client grows it
  // silently to the ring's capacity if the ring is larger (so a full ring record
  // always fits), and per request if a value would not otherwise fit.
  static constexpr std::size_t kDefaultBufferBytes = 16 * 1024;

  [[nodiscard]] static std::optional<SbeRingClient> open(
      const char* path, ms wait = ms(2000), std::size_t buffer_size = kDefaultBufferBytes) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
      if (auto m = ring::Mapping::open(path)) {
        // Reconnect handshake: claim a fresh epoch and wait for the server to drain the
        // ring (discarding whatever a dead predecessor left in flight) and ack it, so we
        // always start from a clean SQ/CQ no matter how the previous client exited.
        const std::uint64_t epoch = m->request_reconnect();
        while (!m->reconnect_acked(epoch)) {
          if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
          ring::cpu_relax();
        }
        SbeRingClient c(std::move(*m), buffer_size);
        c.sq_.send(std::string_view(kGoblinMagicBytes, sizeof(kGoblinMagicBytes)),
                   [] { return false; });
        return c;
      }
      if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
      ring::cpu_relax();
    }
  }

  // Size the buffers hold after construction (max of the configured size and the
  // ring capacity).
  [[nodiscard]] std::size_t buffer_size() const noexcept { return sendbuf_.size(); }

  // ---- connection ------------------------------------------------------------
  [[nodiscard]] bool ping(ms timeout = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Ping>(0);
    finish(m, timeout);
    return reply_is<goblin_sbe::StatusReply>() &&
           decode<goblin_sbe::StatusReply>().getStatusAsStringView() == "PONG";
  }
  [[nodiscard]] std::string echo(std::string_view message, ms timeout = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Echo>(message.size());
    m.putMessage(message.data(), u32(message.size()));
    finish(m, timeout);
    return as_bulk();
  }
  [[nodiscard]] std::string info(ms timeout = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Info>(0);
    finish(m, timeout);
    return as_bulk();
  }

  // ---- strings ---------------------------------------------------------------
  [[nodiscard]] std::optional<std::string> get(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Get>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  SetReply set(std::string_view key, std::string_view value, const SetOptions& o = {},
               ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Set>(key.size() + value.size());
    std::uint8_t flags = 0;
    if (o.nx) flags |= 0x01;
    if (o.xx) flags |= 0x02;
    if (o.get) flags |= 0x04;
    if (o.keepttl) flags |= 0x08;
    m.flags(flags).expireMode(o.expire_mode).expireValue(o.expire_value);
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    throw_if_error();
    SetReply r;
    if (reply_is<goblin_sbe::StatusReply>()) r.ok = true;
    else if (reply_is<goblin_sbe::BulkReply>()) r.old = std::string(decode<goblin_sbe::BulkReply>().getValueAsStringView());
    return r;  // NilReply -> {false, nullopt}
  }
  [[nodiscard]] std::optional<std::string> getset(std::string_view key, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GetSet>(key.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] long long setnx(std::string_view key, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::SetNx>(key.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::optional<std::string> getdel(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GetDel>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] long long strlen(std::string_view key, ms t = kDefaultTimeout) {
    return key_int<goblin_sbe::StrLen>(key, t);
  }
  [[nodiscard]] long long append(std::string_view key, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Append>(key.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long incr(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::Incr>(key, t); }
  [[nodiscard]] long long decr(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::Decr>(key, t); }
  [[nodiscard]] long long incrby(std::string_view key, long long delta, ms t = kDefaultTimeout) {
    return delta_key_int<goblin_sbe::IncrBy>(delta, key, t);
  }
  [[nodiscard]] long long decrby(std::string_view key, long long delta, ms t = kDefaultTimeout) {
    return delta_key_int<goblin_sbe::DecrBy>(delta, key, t);
  }
  [[nodiscard]] std::string incrbyfloat(std::string_view key, double delta, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::IncrByFloat>(key.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk();
  }
  [[nodiscard]] std::string getrange(std::string_view key, long long start, long long end, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GetRange>(key.size());
    m.start(start).end(end);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk();
  }
  [[nodiscard]] long long setrange(std::string_view key, long long offset, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::SetRange>(key.size() + value.size());
    m.byteOffset(offset);
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  void mset(std::span<const std::pair<std::string_view, std::string_view>> pairs, ms t = kDefaultTimeout) {
    std::size_t need = 0;
    for (const auto& [k, v] : pairs) need += k.size() + v.size();
    auto& m = build<goblin_sbe::MSet>(need);
    auto& g = m.pairsCount(u16(pairs.size()));
    for (const auto& [k, v] : pairs)
      g.next().putKey(k.data(), u32(k.size())).putValue(v.data(), u32(v.size()));
    finish(m, t);
    (void)as_status();
  }
  [[nodiscard]] std::vector<std::optional<std::string>> mget(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto& m = key_group<goblin_sbe::MGet>(keys);
    finish(m, t);
    return as_nullable_array();
  }

  // ---- keyspace / TTL --------------------------------------------------------
  [[nodiscard]] long long del(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto& m = key_group<goblin_sbe::Del>(keys);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long exists(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto& m = key_group<goblin_sbe::Exists>(keys);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::string type(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::Type>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_status();
  }
  [[nodiscard]] long long expire(std::string_view key, long long seconds, std::uint8_t flags = 0, ms t = kDefaultTimeout) {
    return expire_impl<goblin_sbe::Expire>(key, seconds, flags, t);
  }
  [[nodiscard]] long long pexpire(std::string_view key, long long ms_, std::uint8_t flags = 0, ms t = kDefaultTimeout) {
    return expire_impl<goblin_sbe::PExpire>(key, ms_, flags, t);
  }
  [[nodiscard]] long long expireat(std::string_view key, long long ts, std::uint8_t flags = 0, ms t = kDefaultTimeout) {
    return expire_impl<goblin_sbe::ExpireAt>(key, ts, flags, t);
  }
  [[nodiscard]] long long pexpireat(std::string_view key, long long ts, std::uint8_t flags = 0, ms t = kDefaultTimeout) {
    return expire_impl<goblin_sbe::PExpireAt>(key, ts, flags, t);
  }
  [[nodiscard]] long long ttl(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::Ttl>(key, t); }
  [[nodiscard]] long long pttl(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::PTtl>(key, t); }
  [[nodiscard]] long long persist(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::Persist>(key, t); }
  [[nodiscard]] long long expiretime(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::ExpireTime>(key, t); }
  [[nodiscard]] long long pexpiretime(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::PExpireTime>(key, t); }

  // ---- hash ------------------------------------------------------------------
  [[nodiscard]] long long hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fv, ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& [f, v] : fv) need += f.size() + v.size();
    auto& m = build<goblin_sbe::HSet>(need);
    auto& g = m.entriesCount(u16(fv.size()));
    for (const auto& [f, v] : fv)
      g.next().putField(f.data(), u32(f.size())).putValue(v.data(), u32(v.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hsetnx(std::string_view key, std::string_view field, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::HSetNx>(key.size() + field.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::optional<std::string> hget(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto& m = key_field<goblin_sbe::HGet>(key, field);
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::vector<std::optional<std::string>> hmget(std::string_view key, std::span<const std::string_view> fields, ms t = kDefaultTimeout) {
    auto& m = field_group<goblin_sbe::HMGet>(key, fields);
    finish(m, t);
    return as_nullable_array();
  }
  [[nodiscard]] long long hdel(std::string_view key, std::span<const std::string_view> fields, ms t = kDefaultTimeout) {
    auto& m = field_group<goblin_sbe::HDel>(key, fields);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> hgetall(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::HGetAll>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    const auto flat = as_array();
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) out.emplace_back(flat[i], flat[i + 1]);
    return out;
  }
  [[nodiscard]] std::vector<std::string> hkeys(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::HKeys>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::vector<std::string> hvals(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::HVals>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] long long hlen(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::HLen>(key, t); }
  [[nodiscard]] long long hexists(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto& m = key_field<goblin_sbe::HExists>(key, field);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hstrlen(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto& m = key_field<goblin_sbe::HStrLen>(key, field);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hincrby(std::string_view key, std::string_view field, long long delta, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::HIncrBy>(key.size() + field.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    finish(m, t);
    return as_int();
  }

  // ---- zset ------------------------------------------------------------------
  [[nodiscard]] long long zadd(std::string_view key, std::span<const Scored> members, ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& [s, mem] : members) need += mem.size();
    auto& m = build<goblin_sbe::ZAdd>(need);
    m.flags(0);
    auto& g = m.membersCount(u16(members.size()));
    for (const auto& [s, mem] : members) g.next().score(s).putMember(mem.data(), u32(mem.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zcard(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::ZCard>(key, t); }
  [[nodiscard]] std::optional<double> zscore(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto& m = key_member<goblin_sbe::ZScore>(key, member);
    finish(m, t);
    return as_double_or_nil();
  }
  [[nodiscard]] std::optional<long long> zrank(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto& m = key_member<goblin_sbe::ZRank>(key, member);
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] std::optional<long long> zrevrank(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto& m = key_member<goblin_sbe::ZRevRank>(key, member);
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] long long zrem(std::string_view key, std::span<const std::string_view> members, ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& mem : members) need += mem.size();
    auto& m = build<goblin_sbe::ZRem>(need);
    auto& g = m.membersCount(u16(members.size()));
    for (const auto& mem : members) g.next().putMember(mem.data(), u32(mem.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zremrangebyscore(std::string_view key, double min, bool min_excl, double max, bool max_excl, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::ZRemRangeByScore>(key.size());
    m.min(min).minExclusive(min_excl ? 1 : 0).max(max).maxExclusive(max_excl ? 1 : 0);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::string> zrange(std::string_view key, long long start, long long stop, bool rev = false, ms t = kDefaultTimeout) {
    auto& m = zrange_build(key, start, stop, false, rev);
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>> zrange_withscores(std::string_view key, long long start, long long stop, bool rev = false, ms t = kDefaultTimeout) {
    auto& m = zrange_build(key, start, stop, true, rev);
    finish(m, t);
    return as_scored_array();
  }

  // ---- GOBLIN.* natives ------------------------------------------------------
  [[nodiscard]] long long cad(std::string_view key, std::string_view token, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinCad>(key.size() + token.size());
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long caexpire(std::string_view key, std::string_view token, long long ms_, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinCaExpire>(key.size() + token.size());
    m.ms(ms_);
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_int();
  }
  // CAS: true on a KEEPTTL swap (+OK), false on a token mismatch (0).
  [[nodiscard]] bool cas(std::string_view key, std::string_view token, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinCas>(key.size() + token.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    throw_if_error();
    return reply_is<goblin_sbe::StatusReply>();
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>> td_leaderboard_rescore(
      std::string_view key, double now, double hl, long long k, std::uint8_t mode, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinTdRescore>(key.size());
    m.now(now).hl(hl).k(k).mode(mode);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_scored_array();
  }
  [[nodiscard]] long long increx(std::string_view key, long long seconds, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinIncrEx>(key.size());
    m.seconds(seconds);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zwindow(std::string_view key, double now, double window, long long limit, std::string_view member, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinZWindow>(key.size() + member.size());
    m.now(now).window(window).limit(limit);
    m.putKey(key.data(), u32(key.size()));
    m.putMember(member.data(), u32(member.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long incrbound(std::string_view key, long long delta, long long max, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinIncrBound>(key.size());
    m.delta(delta).max(max);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long decrpos(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::GoblinDecrPos>(key, t); }
  [[nodiscard]] long long hcad(std::string_view key, std::string_view field, std::string_view expected, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinHCad>(key.size() + field.size() + expected.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putExpected(expected.data(), u32(expected.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hsetgt(std::string_view key, std::string_view field, std::string_view value, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinHSetGt>(key.size() + field.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  // CLAIM: "CLAIMED"/the stored result on a bulk reply, nullopt on nil.
  [[nodiscard]] std::optional<std::string> claim(std::string_view claim_key, std::string_view result_key, std::string_view token, long long seconds, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinClaim>(claim_key.size() + result_key.size() + token.size());
    m.seconds(seconds);
    m.putClaimKey(claim_key.data(), u32(claim_key.size()));
    m.putResultKey(result_key.data(), u32(result_key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }

  // ---- admin -----------------------------------------------------------------
  [[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>> memory(std::string_view key, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinMemory>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_map_or_nil();
  }
  [[nodiscard]] std::optional<long long> optimize(std::string_view key, double density = 0.0, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinOptimize>(key.size());
    m.density(density);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] std::string save(std::string_view path, bool accel = true, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinSave>(path.size());
    m.accel(accel ? 1 : 0);
    m.putPath(path.data(), u32(path.size()));
    finish(m, t);
    return as_status();
  }
  [[nodiscard]] long long load(std::string_view path, ms t = kDefaultTimeout) {
    auto& m = build<goblin_sbe::GoblinLoad>(path.size());
    m.putPath(path.data(), u32(path.size()));
    finish(m, t);
    return as_int();
  }

  // ---- scripting -------------------------------------------------------------
  [[nodiscard]] RespValue eval(std::string_view script, std::span<const std::string_view> keys,
                               std::span<const std::string_view> args, Language lang = Language::lua, ms t = kDefaultTimeout) {
    return eval_impl<goblin_sbe::Eval>(script, keys, args, lang, t, /*is_script=*/false);
  }
  [[nodiscard]] RespValue eval_sha(std::string_view sha, std::span<const std::string_view> keys,
                                   std::span<const std::string_view> args, Language lang = Language::lua, ms t = kDefaultTimeout) {
    return eval_impl<goblin_sbe::EvalSha>(sha, keys, args, lang, t, /*is_script=*/false);
  }
  [[nodiscard]] RespValue script(std::span<const std::string_view> args, Language lang = Language::lua, ms t = kDefaultTimeout) {
    std::size_t need = 0;
    for (const auto& a : args) need += a.size();
    auto& m = build<goblin_sbe::Script>(need);
    m.language(static_cast<std::uint8_t>(lang));
    auto& ag = m.argsCount(u16(args.size()));
    for (const auto& a : args) ag.next().putArg(a.data(), u32(a.size()));
    finish(m, t);
    return as_resp_value();
  }

 private:
  explicit SbeRingClient(ring::Mapping&& m, std::size_t buffer_size)
      : map_(std::move(m)), sq_(map_.sq_producer()), cq_(map_.cq_consumer()) {
    // The configured size is a floor; grow silently to the ring's capacity so a full
    // ring record always fits without a reallocation on the hot path.
    sendbuf_.resize(std::max(buffer_size, static_cast<std::size_t>(map_.sq_capacity())));
    cqbuf_.reserve(std::max(buffer_size, static_cast<std::size_t>(map_.cq_capacity())));
  }

  static std::uint32_t u32(std::size_t n) { return static_cast<std::uint32_t>(n); }
  static std::uint16_t u16(std::size_t n) { return static_cast<std::uint16_t>(n); }

  // Ensure the send buffer holds a request whose var-data totals `payload` bytes.
  template <class Msg>
  Msg& build(std::size_t payload) {
    if (sendbuf_.size() < payload + 512) sendbuf_.resize(payload + 512);
    msg_holder_.template emplace<Msg>();
    Msg& m = std::any_cast<Msg&>(msg_holder_);
    m.wrapAndApplyHeader(sendbuf_.data(), kSbeLenPrefix, sendbuf_.size());
    return m;
  }

  // Common request shapes.
  template <class Msg>
  Msg& key_group(std::span<const std::string_view> keys) {
    std::size_t need = 0;
    for (const auto& k : keys) need += k.size();
    Msg& m = build<Msg>(need);
    auto& g = m.keysCount(u16(keys.size()));
    for (const auto& k : keys) g.next().putKey(k.data(), u32(k.size()));
    return m;
  }
  template <class Msg>
  Msg& field_group(std::string_view key, std::span<const std::string_view> fields) {
    std::size_t need = key.size();
    for (const auto& f : fields) need += f.size();
    Msg& m = build<Msg>(need);
    auto& g = m.fieldsCount(u16(fields.size()));
    for (const auto& f : fields) g.next().putField(f.data(), u32(f.size()));
    m.putKey(key.data(), u32(key.size()));
    return m;
  }
  template <class Msg>
  Msg& key_member(std::string_view key, std::string_view member) {
    Msg& m = build<Msg>(key.size() + member.size());
    m.putKey(key.data(), u32(key.size()));
    m.putMember(member.data(), u32(member.size()));
    return m;
  }
  template <class Msg>
  Msg& key_field(std::string_view key, std::string_view field) {
    Msg& m = build<Msg>(key.size() + field.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    return m;
  }
  template <class Msg>
  long long key_int(std::string_view key, ms t) {
    Msg& m = build<Msg>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  long long delta_key_int(long long delta, std::string_view key, ms t) {
    Msg& m = build<Msg>(key.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  long long expire_impl(std::string_view key, long long amount, std::uint8_t flags, ms t) {
    Msg& m = build<Msg>(key.size());
    m.amount(amount).flags(flags);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  goblin_sbe::ZRange& zrange_build(std::string_view key, long long start, long long stop, bool ws, bool rev) {
    auto& m = build<goblin_sbe::ZRange>(key.size());
    m.start(start).stop(stop).withScores(ws ? 1 : 0).rev(rev ? 1 : 0);
    m.putKey(key.data(), u32(key.size()));
    return m;
  }
  template <class Msg>
  RespValue eval_impl(std::string_view code, std::span<const std::string_view> keys,
                      std::span<const std::string_view> args, Language lang, ms t, bool) {
    std::size_t need = code.size();
    for (const auto& k : keys) need += k.size();
    for (const auto& a : args) need += a.size();
    Msg& m = build<Msg>(need);
    m.language(static_cast<std::uint8_t>(lang));
    auto& kg = m.keysCount(u16(keys.size()));
    for (const auto& k : keys) kg.next().putKey(k.data(), u32(k.size()));
    auto& ag = m.argsCount(u16(args.size()));
    for (const auto& a : args) ag.next().putArg(a.data(), u32(a.size()));
    put_code(m, code);
    finish(m, t);
    return as_resp_value();
  }
  static void put_code(goblin_sbe::Eval& m, std::string_view s) { m.putScript(s.data(), u32(s.size())); }
  static void put_code(goblin_sbe::EvalSha& m, std::string_view s) { m.putSha(s.data(), u32(s.size())); }

  // Frame the just-built message and block for its reply.
  template <class Msg>
  void finish(Msg& msg, ms timeout) {
    const std::uint32_t len =
        static_cast<std::uint32_t>(goblin_sbe::MessageHeader::encodedLength() + msg.encodedLength());
    std::memcpy(sendbuf_.data(), &len, kSbeLenPrefix);
    sq_.send(std::string_view(sendbuf_.data(), kSbeLenPrefix + len), [] { return false; });
    read_frame(timeout);
  }

  // ---- reply decoding --------------------------------------------------------
  [[nodiscard]] goblin_sbe::MessageHeader reply_header() {
    return goblin_sbe::MessageHeader(last_frame_.data(), last_frame_.size());
  }
  template <class Msg>
  [[nodiscard]] bool reply_is() { return reply_header().templateId() == Msg::sbeTemplateId(); }
  template <class Msg>
  [[nodiscard]] Msg decode() {
    const auto h = reply_header();
    Msg m;
    m.wrapForDecode(last_frame_.data(), goblin_sbe::MessageHeader::encodedLength(), h.blockLength(),
                    h.version(), last_frame_.size());
    return m;
  }
  void throw_if_error() {
    if (reply_is<goblin_sbe::ErrorReply>()) {
      auto e = decode<goblin_sbe::ErrorReply>();
      std::string code(e.getCodeAsStringView());
      std::string message(e.getMessageAsStringView());
      throw std::runtime_error(code + " " + message);
    }
  }
  [[noreturn]] void unexpected() { throw std::runtime_error("SbeRingClient: unexpected reply type"); }

  [[nodiscard]] long long as_int() {
    throw_if_error();
    if (!reply_is<goblin_sbe::IntReply>()) unexpected();
    return decode<goblin_sbe::IntReply>().value();
  }
  [[nodiscard]] std::optional<long long> as_int_or_nil() {
    throw_if_error();
    if (reply_is<goblin_sbe::NilReply>()) return std::nullopt;
    if (!reply_is<goblin_sbe::IntReply>()) unexpected();
    return decode<goblin_sbe::IntReply>().value();
  }
  [[nodiscard]] std::optional<double> as_double_or_nil() {
    throw_if_error();
    if (reply_is<goblin_sbe::NilReply>()) return std::nullopt;
    if (!reply_is<goblin_sbe::DoubleReply>()) unexpected();
    return decode<goblin_sbe::DoubleReply>().value();
  }
  [[nodiscard]] std::string as_bulk() {
    throw_if_error();
    if (!reply_is<goblin_sbe::BulkReply>()) unexpected();
    return std::string(decode<goblin_sbe::BulkReply>().getValueAsStringView());
  }
  [[nodiscard]] std::optional<std::string> as_bulk_or_nil() {
    throw_if_error();
    if (reply_is<goblin_sbe::NilReply>()) return std::nullopt;
    if (!reply_is<goblin_sbe::BulkReply>()) unexpected();
    return std::string(decode<goblin_sbe::BulkReply>().getValueAsStringView());
  }
  [[nodiscard]] std::string as_status() {
    throw_if_error();
    if (!reply_is<goblin_sbe::StatusReply>()) unexpected();
    return std::string(decode<goblin_sbe::StatusReply>().getStatusAsStringView());
  }
  [[nodiscard]] std::vector<std::string> as_array() {
    throw_if_error();
    if (!reply_is<goblin_sbe::ArrayReply>()) unexpected();
    auto r = decode<goblin_sbe::ArrayReply>();
    std::vector<std::string> out;
    auto& g = r.items();
    while (g.hasNext()) { g.next(); const auto v = g.getValueAsStringView(); out.emplace_back(v.data(), v.size()); }
    return out;
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>> as_scored_array() {
    throw_if_error();
    if (!reply_is<goblin_sbe::ScoredArrayReply>()) unexpected();
    auto r = decode<goblin_sbe::ScoredArrayReply>();
    std::vector<std::pair<std::string, double>> out;
    auto& g = r.items();
    while (g.hasNext()) { g.next(); const double s = g.score(); const auto m = g.getMemberAsStringView(); out.emplace_back(std::string(m.data(), m.size()), s); }
    return out;
  }
  [[nodiscard]] std::vector<std::optional<std::string>> as_nullable_array() {
    throw_if_error();
    if (!reply_is<goblin_sbe::NullableArrayReply>()) unexpected();
    auto r = decode<goblin_sbe::NullableArrayReply>();
    std::vector<std::optional<std::string>> out;
    auto& g = r.items();
    while (g.hasNext()) {
      g.next();
      const bool present = g.present() != 0;
      const auto v = g.getValueAsStringView();
      if (present) out.emplace_back(std::string(v.data(), v.size()));
      else out.emplace_back(std::nullopt);
    }
    return out;
  }
  [[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>> as_map_or_nil() {
    throw_if_error();
    if (reply_is<goblin_sbe::NilReply>()) return std::nullopt;
    if (!reply_is<goblin_sbe::MapReply>()) unexpected();
    auto r = decode<goblin_sbe::MapReply>();
    std::vector<std::pair<std::string, std::string>> out;
    auto& g = r.entries();
    while (g.hasNext()) {
      g.next();
      const auto k = g.getKeyAsStringView();
      const auto v = g.getValueAsStringView();
      out.emplace_back(std::string(k.data(), k.size()), std::string(v.data(), v.size()));
    }
    return out;
  }
  [[nodiscard]] RespValue as_resp_value() {
    // A script reply is never an ErrorReply frame -- a script error is an `error`
    // node inside the RespValueReply -- so decode the node tree directly.
    if (!reply_is<goblin_sbe::RespValueReply>()) { throw_if_error(); unexpected(); }
    auto r = decode<goblin_sbe::RespValueReply>();
    std::vector<Node> nodes;
    auto& g = r.nodes();
    while (g.hasNext()) {
      g.next();
      Node n;
      n.type = g.type();
      n.int_value = g.intValue();
      n.child_count = g.childCount();
      const auto b = g.getBytesAsStringView();
      n.bytes.assign(b.data(), b.size());
      nodes.push_back(std::move(n));
    }
    std::size_t i = 0;
    return nodes.empty() ? RespValue{} : rebuild(nodes, i);
  }

  struct Node { std::uint8_t type; std::int64_t int_value; std::uint32_t child_count; std::string bytes; };
  static RespValue rebuild(const std::vector<Node>& nodes, std::size_t& i) {
    const Node& n = nodes[i++];
    RespValue v;
    switch (n.type) {
      case 1: v.type = RespValue::Type::integer; v.integer = n.int_value; break;
      case 2: v.type = RespValue::Type::bulk; v.str = n.bytes; break;
      case 3: v.type = RespValue::Type::status; v.str = n.bytes; break;
      case 4: v.type = RespValue::Type::error; v.str = n.bytes; break;
      case 5: {  // array
        v.type = RespValue::Type::array;
        for (std::uint32_t c = 0; c < n.child_count && i < nodes.size(); ++c) v.elements.push_back(rebuild(nodes, i));
        break;
      }
      case 6: {  // map -> flattened key,value,...
        v.type = RespValue::Type::array;
        for (std::uint32_t c = 0; c < n.child_count * 2 && i < nodes.size(); ++c) v.elements.push_back(rebuild(nodes, i));
        break;
      }
      default: v.type = RespValue::Type::nil; break;
    }
    return v;
  }

  void read_frame(ms timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      if (const auto rec = cq_.peek()) { cqbuf_.append(*rec); cq_.pop(); }
      if (cqbuf_.size() >= kSbeLenPrefix) {
        std::uint32_t len = 0;
        std::memcpy(&len, cqbuf_.data(), kSbeLenPrefix);
        if (cqbuf_.size() >= kSbeLenPrefix + len) {
          last_frame_.assign(cqbuf_.data() + kSbeLenPrefix, len);
          cqbuf_.erase(0, kSbeLenPrefix + len);
          return;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("SbeRingClient: timed out waiting for a reply");
      ring::cpu_relax();
    }
  }

  ring::Mapping map_;
  ring::Producer sq_;
  ring::Consumer cq_;
  std::vector<char> sendbuf_;  // growable request buffer (values up to 64 KiB)
  std::string cqbuf_;          // CQ byte accumulator
  std::string last_frame_;     // the last reply's SBE message (prefix stripped)
  std::any msg_holder_;        // holds the flyweight for the in-flight request
};

}  // namespace goblin::core
