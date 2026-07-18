#pragma once

// A compile-time-transport C++ SBE client covering the full command surface.
// It includes the generated SBE codecs (header-only -- no link dependency) and
// speaks the length-prefixed SBE framing (see sbe_frame.hpp): it sends the
// GOBLINS! magic once on open, then builds typed requests and decodes typed
// replies. Calls may be synchronous or explicitly enqueued and read back in
// order. An ErrorReply becomes a thrown std::runtime_error ("<code> <message>").
//
// Every request builds into a growable send buffer (logical values may be larger
// when server-side LZ4 is enabled). Shared-memory requests are published as one
// complete ring record for in-place server decode; oversized messages are rejected.
// Replies are decoded straight out of the last received frame.

#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin/core/sbe_socket_transport.hpp"
#if defined(GOBLIN_HAS_RDMA)
#include "goblin/core/rdma_ring.hpp"
#endif
#if defined(GOBLIN_HAS_EXASOCK)
#include "goblin/core/exasock_transport.hpp"
#endif

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
#include "goblin_sbe/PubSubNumSubReply.h"
#include "goblin_sbe/PubSubPush.h"
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
// List
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
// Sets
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
#include "goblin_sbe/PSubscribe.h"
#include "goblin_sbe/PUnsubscribe.h"
#include "goblin_sbe/PubSub.h"
#include "goblin_sbe/Publish.h"
#include "goblin_sbe/Subscribe.h"
#include "goblin_sbe/Unsubscribe.h"
// Scripting
#include "goblin_sbe/Eval.h"
#include "goblin_sbe/EvalSha.h"
#include "goblin_sbe/Script.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
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

// Selects the representation only when a push creates a new list. Existing lists
// retain their representation; `selected` follows the server's --list-implementation.
enum class SbeListImplementation : std::uint8_t {
  selected = 0,
  pma = 1,
  segmented = 2,
};

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

enum class PubSubKind : std::uint8_t {
  message = 0,
  pattern_message = 1,
  subscribe = 2,
  pattern_subscribe = 3,
  unsubscribe = 4,
  pattern_unsubscribe = 5,
};

struct PubSubMessage {
  PubSubKind kind{PubSubKind::message};
  std::uint32_t subscription_count{0};
  std::string pattern;
  std::string channel;
  std::string payload;
};

// Adapts the existing shared-memory mapping to the same compile-time transport
// surface as the one-sided RDMA connection. The SBE client below consequently
// has no virtual dispatch and keeps one implementation of every typed command.
class RingSbeTransport {
 public:
  using ms = std::chrono::milliseconds;

  [[nodiscard]] static std::optional<RingSbeTransport> open(
      const char* path, ms wait = ms(2000),
      std::size_t buffer_size = 16 * 1024) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
      if (auto mapping = ring::Mapping::open(path)) {
        const std::uint64_t epoch = mapping->request_reconnect();
        while (!mapping->reconnect_acked(epoch)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
          }
          ring::cpu_relax();
        }
        return RingSbeTransport(std::move(*mapping), buffer_size);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      ring::cpu_relax();
    }
  }

  template <class StopFn>
  bool send(std::string_view bytes, StopFn&& stop) {
    return producer_.send_record(bytes, std::forward<StopFn>(stop));
  }
  [[nodiscard]] std::optional<std::string_view> peek() noexcept {
    return consumer_.peek();
  }
  void pop() noexcept { consumer_.pop(); }
  void wait_for_record() noexcept { consumer_.wait_for_record(); }
  [[nodiscard]] std::size_t send_capacity() const noexcept {
    return mapping_.sq_capacity();
  }
  [[nodiscard]] std::size_t receive_capacity() const noexcept {
    return mapping_.cq_capacity();
  }
  [[nodiscard]] std::size_t max_message_bytes() const noexcept {
    return producer_.max_record_payload();
  }
  [[nodiscard]] std::size_t buffer_size_hint() const noexcept {
    return buffer_size_;
  }

 private:
  RingSbeTransport(ring::Mapping&& mapping, std::size_t buffer_size) noexcept
      : mapping_(std::move(mapping)),
        producer_(mapping_.sq_producer()),
        consumer_(mapping_.cq_consumer()),
        buffer_size_(buffer_size) {}

  ring::Mapping mapping_;
  ring::Producer producer_;
  ring::Consumer consumer_;
  std::size_t buffer_size_{0};
};

template <class Transport>
class BasicSbeClient {
 public:
  using Scored = std::pair<double, std::string_view>;
  using ms = std::chrono::milliseconds;
  static constexpr ms kDefaultTimeout{5000};
  // Initial size of the request/reply buffers. It is a floor: the client grows it
  // to the transport capacity and per request when a value would not otherwise
  // fit.
  static constexpr std::size_t kDefaultBufferBytes = 16 * 1024;

  template <class... Args>
  [[nodiscard]] static std::optional<BasicSbeClient> open(Args&&... args) {
    auto transport = Transport::open(std::forward<Args>(args)...);
    if (!transport) {
      return std::nullopt;
    }
    BasicSbeClient client(std::move(*transport));
    if (!client.transport_.send(
            std::string_view(kGoblinMagicBytes, sizeof(kGoblinMagicBytes)),
            [] { return false; })) {
      return std::nullopt;
    }
    return client;
  }

  // Size the buffers hold after construction (max of the configured size and the
  // ring capacity).
  [[nodiscard]] std::size_t buffer_size() const noexcept { return sendbuf_.size(); }
  [[nodiscard]] std::size_t max_message_bytes() const noexcept {
    return transport_.max_message_bytes();
  }

  // Pipeline requests are written as complete SBE messages and replies are read
  // in submission order. SBE needs no correlation id because one connection is
  // single-threaded and the server emits one ordinary reply per request, in order.
  // `payload_bytes` is the variable-data budget used to size the request buffer;
  // `encode` fills the generated SBE flyweight.
  template <class Message, class Encode>
  void enqueue_sbe(std::size_t payload_bytes, Encode&& encode,
                   ms timeout = kDefaultTimeout) {
    auto message = build<Message>(payload_bytes);
    std::forward<Encode>(encode)(message);
    enqueue_built(message, timeout);
  }

  [[nodiscard]] std::size_t outstanding_pipeline_replies() const noexcept {
    return outstanding_pipeline_replies_;
  }

  // The returned view is the SBE message without its uint32 length prefix and is
  // valid until the next read. Typed readers below consume the same next frame.
  [[nodiscard]] std::string_view read_pipeline_frame(
      ms timeout = kDefaultTimeout) {
    if (outstanding_pipeline_replies_ == 0) {
      throw std::logic_error(
          "SbeClient: no pipelined reply is outstanding");
    }
    read_frame(timeout);
    --outstanding_pipeline_replies_;
    return last_frame_;
  }

  [[nodiscard]] long long read_pipeline_int(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_int();
  }
  [[nodiscard]] std::optional<long long> read_pipeline_int_or_nil(
      ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_int_or_nil();
  }
  [[nodiscard]] std::optional<double> read_pipeline_double_or_nil(
      ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_double_or_nil();
  }
  [[nodiscard]] std::string read_pipeline_bulk(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_bulk();
  }
  [[nodiscard]] std::optional<std::string> read_pipeline_bulk_or_nil(
      ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::string read_pipeline_status(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_status();
  }
  [[nodiscard]] std::vector<std::string> read_pipeline_array(
      ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_array();
  }
  [[nodiscard]] std::optional<std::vector<std::string>>
  read_pipeline_array_or_nil(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_array_or_nil();
  }
  [[nodiscard]] std::vector<std::optional<std::string>>
  read_pipeline_nullable_array(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_nullable_array();
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>>
  read_pipeline_scored_array(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_scored_array();
  }
  [[nodiscard]] std::optional<
      std::vector<std::pair<std::string, std::string>>>
  read_pipeline_map_or_nil(ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_map_or_nil();
  }
  [[nodiscard]] RespValue read_pipeline_resp_value(
      ms timeout = kDefaultTimeout) {
    (void)read_pipeline_frame(timeout);
    return as_resp_value();
  }

  // ---- connection ------------------------------------------------------------
  [[nodiscard]] bool ping(ms timeout = kDefaultTimeout) {
    auto m = build<goblin_sbe::Ping>(0);
    finish(m, timeout);
    // Single header walk (reply_is + decode each re-parse the header).
    const auto h = reply_header();
    if (h.templateId() != goblin_sbe::StatusReply::sbeTemplateId()) return false;
    goblin_sbe::StatusReply r;
    r.wrapForDecode(last_frame_.data(), goblin_sbe::MessageHeader::encodedLength(),
                    h.blockLength(), h.version(), last_frame_.size());
    return r.getStatusAsStringView() == "PONG";
  }
  [[nodiscard]] std::string echo(std::string_view message, ms timeout = kDefaultTimeout) {
    auto m = build<goblin_sbe::Echo>(message.size());
    m.putMessage(message.data(), u32(message.size()));
    finish(m, timeout);
    return as_bulk();
  }
  [[nodiscard]] std::string info(ms timeout = kDefaultTimeout) {
    auto m = build<goblin_sbe::Info>(0);
    finish(m, timeout);
    return as_bulk();
  }

  // ---- Pub/Sub ---------------------------------------------------------------
  [[nodiscard]] std::vector<PubSubMessage> subscribe(
      std::span<const std::string_view> channels, ms timeout = kDefaultTimeout) {
    require_pubsub_names(channels, "subscribe");
    auto message = build<goblin_sbe::Subscribe>(total_bytes(channels));
    auto& group = message.channelsCount(u16(channels.size()));
    for (const auto channel : channels) {
      group.next().putChannel(channel.data(), u32(channel.size()));
    }
    return finish_pubsub(message, channels.size(), timeout);
  }

  [[nodiscard]] std::vector<PubSubMessage> unsubscribe(
      std::span<const std::string_view> channels = {},
      ms timeout = kDefaultTimeout) {
    require_pubsub_count(channels, "unsubscribe");
    auto message = build<goblin_sbe::Unsubscribe>(total_bytes(channels));
    auto& group = message.channelsCount(u16(channels.size()));
    for (const auto channel : channels) {
      group.next().putChannel(channel.data(), u32(channel.size()));
    }
    const std::size_t expected = channels.empty()
                                     ? std::max<std::size_t>(literal_subscriptions_, 1)
                                     : channels.size();
    return finish_pubsub(message, expected, timeout);
  }

  [[nodiscard]] std::vector<PubSubMessage> psubscribe(
      std::span<const std::string_view> patterns, ms timeout = kDefaultTimeout) {
    require_pubsub_names(patterns, "psubscribe");
    auto message = build<goblin_sbe::PSubscribe>(total_bytes(patterns));
    auto& group = message.patternsCount(u16(patterns.size()));
    for (const auto pattern : patterns) {
      group.next().putPattern(pattern.data(), u32(pattern.size()));
    }
    return finish_pubsub(message, patterns.size(), timeout);
  }

  [[nodiscard]] std::vector<PubSubMessage> punsubscribe(
      std::span<const std::string_view> patterns = {},
      ms timeout = kDefaultTimeout) {
    require_pubsub_count(patterns, "punsubscribe");
    auto message = build<goblin_sbe::PUnsubscribe>(total_bytes(patterns));
    auto& group = message.patternsCount(u16(patterns.size()));
    for (const auto pattern : patterns) {
      group.next().putPattern(pattern.data(), u32(pattern.size()));
    }
    const std::size_t expected = patterns.empty()
                                     ? std::max<std::size_t>(pattern_subscriptions_, 1)
                                     : patterns.size();
    return finish_pubsub(message, expected, timeout);
  }

  [[nodiscard]] long long publish(std::string_view channel,
                                  std::string_view payload,
                                  ms timeout = kDefaultTimeout) {
    auto message = build<goblin_sbe::Publish>(channel.size() + payload.size());
    message.putChannel(channel.data(), u32(channel.size()));
    message.putPayload(payload.data(), u32(payload.size()));
    finish(message, timeout);
    return as_int();
  }

  [[nodiscard]] std::vector<std::string> pubsub_channels(
      std::optional<std::string_view> pattern = std::nullopt,
      ms timeout = kDefaultTimeout) {
    auto message = build<goblin_sbe::PubSub>(pattern ? pattern->size() : 0);
    message.operation(0);
    auto& args = message.argsCount(pattern ? 1 : 0);
    if (pattern) {
      args.next().putArg(pattern->data(), u32(pattern->size()));
    }
    finish(message, timeout);
    return as_array();
  }

  [[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>> pubsub_numsub(
      std::span<const std::string_view> channels,
      ms timeout = kDefaultTimeout) {
    require_pubsub_count(channels, "pubsub_numsub");
    auto message = build_pubsub(1, channels);
    finish(message, timeout);
    throw_if_error();
    if (!reply_is<goblin_sbe::PubSubNumSubReply>()) unexpected();
    auto reply = decode<goblin_sbe::PubSubNumSubReply>();
    auto& items = reply.items();
    std::vector<std::pair<std::string, std::uint32_t>> result;
    result.reserve(static_cast<std::size_t>(items.count()));
    while (items.hasNext()) {
      auto& item = items.next();
      const auto count = item.subscriberCount();
      result.emplace_back(item.getChannelAsStringView(), count);
    }
    return result;
  }

  [[nodiscard]] long long pubsub_numpat(ms timeout = kDefaultTimeout) {
    auto message = build_pubsub(2, {});
    finish(message, timeout);
    return as_int();
  }

  [[nodiscard]] std::optional<PubSubMessage> try_read_pubsub() {
    require_no_pipeline();
    if (!pending_pubsub_.empty()) {
      auto message = std::move(pending_pubsub_.front());
      pending_pubsub_.pop_front();
      return message;
    }
    std::exception_ptr error;
    if (!drain_ready_records(error)) {
      std::rethrow_exception(error);
    }
    if (!extract_buffered_frame()) {
      return std::nullopt;
    }
    if (!reply_is<goblin_sbe::PubSubPush>()) {
      throw std::runtime_error("SbeClient: unexpected synchronous reply while reading Pub/Sub");
    }
    return decode_pubsub();
  }

  [[nodiscard]] PubSubMessage read_pubsub(ms timeout = kDefaultTimeout) {
    if (auto ready = try_read_pubsub()) {
      return std::move(*ready);
    }
    read_frame(timeout, true);
    if (!reply_is<goblin_sbe::PubSubPush>()) unexpected();
    return decode_pubsub();
  }

  // ---- strings ---------------------------------------------------------------
  [[nodiscard]] std::optional<std::string> get(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::Get>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  SetReply set(std::string_view key, std::string_view value, const SetOptions& o = {},
               ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::Set>(key.size() + value.size());
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
    auto m = build<goblin_sbe::GetSet>(key.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] long long setnx(std::string_view key, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SetNx>(key.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::optional<std::string> getdel(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GetDel>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] long long strlen(std::string_view key, ms t = kDefaultTimeout) {
    return key_int<goblin_sbe::StrLen>(key, t);
  }
  [[nodiscard]] long long append(std::string_view key, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::Append>(key.size() + value.size());
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
    auto m = build<goblin_sbe::IncrByFloat>(key.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk();
  }
  [[nodiscard]] std::string getrange(std::string_view key, long long start, long long end, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GetRange>(key.size());
    m.start(start).end(end);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk();
  }
  [[nodiscard]] long long setrange(std::string_view key, long long offset, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SetRange>(key.size() + value.size());
    m.byteOffset(offset);
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  void mset(std::span<const std::pair<std::string_view, std::string_view>> pairs, ms t = kDefaultTimeout) {
    std::size_t need = 0;
    for (const auto& [k, v] : pairs) need += k.size() + v.size();
    auto m = build<goblin_sbe::MSet>(need);
    auto& g = m.pairsCount(u16(pairs.size()));
    for (const auto& [k, v] : pairs)
      g.next().putKey(k.data(), u32(k.size())).putValue(v.data(), u32(v.size()));
    finish(m, t);
    (void)as_status();
  }
  [[nodiscard]] std::vector<std::optional<std::string>> mget(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto m = key_group<goblin_sbe::MGet>(keys);
    finish(m, t);
    return as_nullable_array();
  }

  // ---- keyspace / TTL --------------------------------------------------------
  [[nodiscard]] long long del(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto m = key_group<goblin_sbe::Del>(keys);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long exists(std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    auto m = key_group<goblin_sbe::Exists>(keys);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::string type(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::Type>(key.size());
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
    auto m = build<goblin_sbe::HSet>(need);
    auto& g = m.entriesCount(u16(fv.size()));
    for (const auto& [f, v] : fv)
      g.next().putField(f.data(), u32(f.size())).putValue(v.data(), u32(v.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hsetnx(std::string_view key, std::string_view field, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::HSetNx>(key.size() + field.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::optional<std::string> hget(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto m = key_field<goblin_sbe::HGet>(key, field);
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::vector<std::optional<std::string>> hmget(std::string_view key, std::span<const std::string_view> fields, ms t = kDefaultTimeout) {
    auto m = field_group<goblin_sbe::HMGet>(key, fields);
    finish(m, t);
    return as_nullable_array();
  }
  [[nodiscard]] long long hdel(std::string_view key, std::span<const std::string_view> fields, ms t = kDefaultTimeout) {
    auto m = field_group<goblin_sbe::HDel>(key, fields);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> hgetall(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::HGetAll>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    const auto flat = as_array();
    std::vector<std::pair<std::string, std::string>> out;
    for (std::size_t i = 0; i + 1 < flat.size(); i += 2) out.emplace_back(flat[i], flat[i + 1]);
    return out;
  }
  [[nodiscard]] std::vector<std::string> hkeys(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::HKeys>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::vector<std::string> hvals(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::HVals>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] long long hlen(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::HLen>(key, t); }
  [[nodiscard]] long long hexists(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto m = key_field<goblin_sbe::HExists>(key, field);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hstrlen(std::string_view key, std::string_view field, ms t = kDefaultTimeout) {
    auto m = key_field<goblin_sbe::HStrLen>(key, field);
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hincrby(std::string_view key, std::string_view field, long long delta, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::HIncrBy>(key.size() + field.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    finish(m, t);
    return as_int();
  }

  // Windowed pipeline: keep up to `depth` requests outstanding (K). RESP
  // harnesses commonly use depth 256; SBE used to stay at depth 1. `enqueue(i)`
  // must call one enqueue_* method; `read(i)` must consume one pipelined reply
  // in the same order. Depth 1 reduces to classic request/response.
  template <class EnqueueFn, class ReadFn>
  void pipeline_for(std::size_t count, std::size_t depth, EnqueueFn&& enqueue,
                    ReadFn&& read) {
    if (depth == 0) {
      throw std::invalid_argument("SbeClient: pipeline depth must be >= 1");
    }
    std::size_t issued = 0;
    std::size_t completed = 0;
    while (completed < count) {
      while (issued < count &&
             (issued - completed) < depth) {
        std::forward<EnqueueFn>(enqueue)(issued);
        ++issued;
      }
      std::forward<ReadFn>(read)(completed);
      ++completed;
    }
  }

  // Hash request writers for typed, in-order SBE pipelines. The reply
  // reader must match the command shape (for example HSET -> read_pipeline_int,
  // HGET -> read_pipeline_bulk_or_nil) and calls must remain in enqueue order.
  void enqueue_hset(
      std::string_view key,
      std::span<const std::pair<std::string_view, std::string_view>> fv,
      ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& [field, value] : fv) {
      need += field.size() + value.size();
    }
    auto message = build<goblin_sbe::HSet>(need);
    auto& entries = message.entriesCount(u16(fv.size()));
    for (const auto& [field, value] : fv) {
      entries.next()
          .putField(field.data(), u32(field.size()))
          .putValue(value.data(), u32(value.size()));
    }
    message.putKey(key.data(), u32(key.size()));
    enqueue_built(message, t);
  }

  void enqueue_hsetnx(std::string_view key, std::string_view field,
                      std::string_view value, ms t = kDefaultTimeout) {
    auto message =
        build<goblin_sbe::HSetNx>(key.size() + field.size() + value.size());
    message.putKey(key.data(), u32(key.size()));
    message.putField(field.data(), u32(field.size()));
    message.putValue(value.data(), u32(value.size()));
    enqueue_built(message, t);
  }

  void enqueue_hget(std::string_view key, std::string_view field,
                    ms t = kDefaultTimeout) {
    auto message = key_field<goblin_sbe::HGet>(key, field);
    enqueue_built(message, t);
  }

  void enqueue_hmget(std::string_view key,
                     std::span<const std::string_view> fields,
                     ms t = kDefaultTimeout) {
    auto message = field_group<goblin_sbe::HMGet>(key, fields);
    enqueue_built(message, t);
  }

  void enqueue_hdel(std::string_view key,
                    std::span<const std::string_view> fields,
                    ms t = kDefaultTimeout) {
    auto message = field_group<goblin_sbe::HDel>(key, fields);
    enqueue_built(message, t);
  }

  void enqueue_hgetall(std::string_view key, ms t = kDefaultTimeout) {
    auto message = build<goblin_sbe::HGetAll>(key.size());
    message.putKey(key.data(), u32(key.size()));
    enqueue_built(message, t);
  }

  void enqueue_hkeys(std::string_view key, ms t = kDefaultTimeout) {
    auto message = build<goblin_sbe::HKeys>(key.size());
    message.putKey(key.data(), u32(key.size()));
    enqueue_built(message, t);
  }

  void enqueue_hvals(std::string_view key, ms t = kDefaultTimeout) {
    auto message = build<goblin_sbe::HVals>(key.size());
    message.putKey(key.data(), u32(key.size()));
    enqueue_built(message, t);
  }

  void enqueue_hlen(std::string_view key, ms t = kDefaultTimeout) {
    auto message = build<goblin_sbe::HLen>(key.size());
    message.putKey(key.data(), u32(key.size()));
    enqueue_built(message, t);
  }

  void enqueue_hexists(std::string_view key, std::string_view field,
                       ms t = kDefaultTimeout) {
    auto message = key_field<goblin_sbe::HExists>(key, field);
    enqueue_built(message, t);
  }

  void enqueue_hstrlen(std::string_view key, std::string_view field,
                       ms t = kDefaultTimeout) {
    auto message = key_field<goblin_sbe::HStrLen>(key, field);
    enqueue_built(message, t);
  }

  void enqueue_hincrby(std::string_view key, std::string_view field,
                       long long delta, ms t = kDefaultTimeout) {
    auto message = build<goblin_sbe::HIncrBy>(key.size() + field.size());
    message.delta(delta);
    message.putKey(key.data(), u32(key.size()));
    message.putField(field.data(), u32(field.size()));
    enqueue_built(message, t);
  }

  // ---- list ------------------------------------------------------------------
  [[nodiscard]] long long lpush(
      std::string_view key, std::span<const std::string_view> values,
      SbeListImplementation implementation = SbeListImplementation::selected,
      bool only_if_exists = false, ms t = kDefaultTimeout) {
    return list_push<goblin_sbe::LPush>(key, values, implementation,
                                        only_if_exists, t);
  }
  [[nodiscard]] long long rpush(
      std::string_view key, std::span<const std::string_view> values,
      SbeListImplementation implementation = SbeListImplementation::selected,
      bool only_if_exists = false, ms t = kDefaultTimeout) {
    return list_push<goblin_sbe::RPush>(key, values, implementation,
                                        only_if_exists, t);
  }
  [[nodiscard]] std::optional<std::string> lpop(
      std::string_view key, ms t = kDefaultTimeout) {
    return list_pop_one<goblin_sbe::LPop>(key, t);
  }
  [[nodiscard]] std::optional<std::string> rpop(
      std::string_view key, ms t = kDefaultTimeout) {
    return list_pop_one<goblin_sbe::RPop>(key, t);
  }
  [[nodiscard]] std::optional<std::vector<std::string>> lpop(
      std::string_view key, std::size_t count, ms t = kDefaultTimeout) {
    return list_pop_many<goblin_sbe::LPop>(key, count, t);
  }
  [[nodiscard]] std::optional<std::vector<std::string>> rpop(
      std::string_view key, std::size_t count, ms t = kDefaultTimeout) {
    return list_pop_many<goblin_sbe::RPop>(key, count, t);
  }
  [[nodiscard]] long long llen(std::string_view key, ms t = kDefaultTimeout) {
    return key_int<goblin_sbe::LLen>(key, t);
  }
  [[nodiscard]] std::optional<std::string> lindex(
      std::string_view key, long long index, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LIndex>(key.size());
    m.index(index);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::vector<std::string> lrange(
      std::string_view key, long long start, long long stop,
      ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LRange>(key.size());
    m.start(start).stop(stop);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  void lset(std::string_view key, long long index, std::string_view value,
            ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LSet>(key.size() + value.size());
    m.index(index);
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    (void)as_status();
  }
  void ltrim(std::string_view key, long long start, long long stop,
             ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LTrim>(key.size());
    m.start(start).stop(stop);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    (void)as_status();
  }
  [[nodiscard]] long long lrem(std::string_view key, long long count,
                               std::string_view value,
                               ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LRem>(key.size() + value.size());
    m.count(count);
    m.putKey(key.data(), u32(key.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long linsert(std::string_view key, bool before,
                                  std::string_view pivot,
                                  std::string_view value,
                                  ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::LInsert>(key.size() + pivot.size() + value.size());
    m.before(before ? 1 : 0);
    m.putKey(key.data(), u32(key.size()));
    m.putPivot(pivot.data(), u32(pivot.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }

  // ---- set -------------------------------------------------------------------
  [[nodiscard]] long long sadd(std::string_view key,
                               std::span<const std::string_view> members,
                               ms t = kDefaultTimeout) {
    return set_members_int<goblin_sbe::SAdd>(key, members, t);
  }
  [[nodiscard]] long long srem(std::string_view key,
                               std::span<const std::string_view> members,
                               ms t = kDefaultTimeout) {
    return set_members_int<goblin_sbe::SRem>(key, members, t);
  }
  [[nodiscard]] long long scard(std::string_view key,
                                ms t = kDefaultTimeout) {
    return key_int<goblin_sbe::SCard>(key, t);
  }
  [[nodiscard]] long long sismember(std::string_view key,
                                    std::string_view member,
                                    ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SIsMember>(key.size() + member.size());
    m.putKey(key.data(), u32(key.size()));
    m.putMember(member.data(), u32(member.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::string> smismember(
      std::string_view key, std::span<const std::string_view> members,
      ms t = kDefaultTimeout) {
    auto m = set_members_msg<goblin_sbe::SMIsMember>(key, members);
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::vector<std::string> smembers(std::string_view key,
                                                  ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SMembers>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::optional<std::string> spop(std::string_view key,
                                                ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SPop>(key.size());
    m.count(-1);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::vector<std::string> spop(std::string_view key,
                                              std::size_t count,
                                              ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SPop>(key.size());
    m.count(static_cast<std::int64_t>(count));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::optional<std::string> srandmember(
      std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SRandMember>(key.size());
    m.count(-1);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  [[nodiscard]] std::vector<std::string> srandmember(
      std::string_view key, long long count, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SRandMember>(key.size());
    m.count(count);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] long long smove(std::string_view source,
                                std::string_view destination,
                                std::string_view member,
                                ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SMove>(source.size() + destination.size() +
                                      member.size());
    m.putSource(source.data(), u32(source.size()));
    m.putDestination(destination.data(), u32(destination.size()));
    m.putMember(member.data(), u32(member.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::string> sinter(
      std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    return set_keys_array<goblin_sbe::SInter>(keys, t);
  }
  [[nodiscard]] std::vector<std::string> sunion(
      std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    return set_keys_array<goblin_sbe::SUnion>(keys, t);
  }
  [[nodiscard]] std::vector<std::string> sdiff(
      std::span<const std::string_view> keys, ms t = kDefaultTimeout) {
    return set_keys_array<goblin_sbe::SDiff>(keys, t);
  }
  [[nodiscard]] long long sinterstore(std::string_view destination,
                                      std::span<const std::string_view> keys,
                                      ms t = kDefaultTimeout) {
    return set_store_int<goblin_sbe::SInterStore>(destination, keys, t);
  }
  [[nodiscard]] long long sunionstore(std::string_view destination,
                                      std::span<const std::string_view> keys,
                                      ms t = kDefaultTimeout) {
    return set_store_int<goblin_sbe::SUnionStore>(destination, keys, t);
  }
  [[nodiscard]] long long sdiffstore(std::string_view destination,
                                     std::span<const std::string_view> keys,
                                     ms t = kDefaultTimeout) {
    return set_store_int<goblin_sbe::SDiffStore>(destination, keys, t);
  }
  [[nodiscard]] long long sintercard(std::span<const std::string_view> keys,
                                     std::size_t limit = 0,
                                     ms t = kDefaultTimeout) {
    std::size_t need = 0;
    for (const auto key : keys) need += key.size();
    auto m = build<goblin_sbe::SInterCard>(need);
    m.limit(static_cast<std::int64_t>(limit));
    auto& g = m.keysCount(u16(keys.size()));
    for (const auto key : keys) {
      g.next().putKey(key.data(), u32(key.size()));
    }
    finish(m, t);
    return as_int();
  }
  // Flat reply: [next_cursor, member...].
  [[nodiscard]] std::vector<std::string> sscan(
      std::string_view key, std::uint64_t cursor, std::size_t count = 10,
      std::string_view match = {}, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::SScan>(key.size() + match.size());
    m.cursor(cursor).count(static_cast<std::int64_t>(count));
    m.putKey(key.data(), u32(key.size()));
    m.putMatch(match.data(), u32(match.size()));
    finish(m, t);
    return as_array();
  }

  // ---- zset ------------------------------------------------------------------
  [[nodiscard]] long long zadd(std::string_view key, std::span<const Scored> members, ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& [s, mem] : members) need += mem.size();
    auto m = build<goblin_sbe::ZAdd>(need);
    m.flags(0);
    auto& g = m.membersCount(u16(members.size()));
    for (const auto& [s, mem] : members) g.next().score(s).putMember(mem.data(), u32(mem.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zcard(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::ZCard>(key, t); }
  [[nodiscard]] std::optional<double> zscore(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto m = key_member<goblin_sbe::ZScore>(key, member);
    finish(m, t);
    return as_double_or_nil();
  }
  [[nodiscard]] std::optional<long long> zrank(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto m = key_member<goblin_sbe::ZRank>(key, member);
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] std::optional<long long> zrevrank(std::string_view key, std::string_view member, ms t = kDefaultTimeout) {
    auto m = key_member<goblin_sbe::ZRevRank>(key, member);
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] long long zrem(std::string_view key, std::span<const std::string_view> members, ms t = kDefaultTimeout) {
    std::size_t need = key.size();
    for (const auto& mem : members) need += mem.size();
    auto m = build<goblin_sbe::ZRem>(need);
    auto& g = m.membersCount(u16(members.size()));
    for (const auto& mem : members) g.next().putMember(mem.data(), u32(mem.size()));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zremrangebyscore(std::string_view key, double min, bool min_excl, double max, bool max_excl, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::ZRemRangeByScore>(key.size());
    m.min(min).minExclusive(min_excl ? 1 : 0).max(max).maxExclusive(max_excl ? 1 : 0);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] std::vector<std::string> zrange(std::string_view key, long long start, long long stop, bool rev = false, ms t = kDefaultTimeout) {
    auto m = zrange_build(key, start, stop, false, rev);
    finish(m, t);
    return as_array();
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>> zrange_withscores(std::string_view key, long long start, long long stop, bool rev = false, ms t = kDefaultTimeout) {
    auto m = zrange_build(key, start, stop, true, rev);
    finish(m, t);
    return as_scored_array();
  }

  // ---- GOBLIN.* natives ------------------------------------------------------
  [[nodiscard]] long long cad(std::string_view key, std::string_view token, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinCad>(key.size() + token.size());
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long caexpire(std::string_view key, std::string_view token, long long ms_, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinCaExpire>(key.size() + token.size());
    m.ms(ms_);
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_int();
  }
  // CAS: true on a KEEPTTL swap (+OK), false on a token mismatch (0).
  [[nodiscard]] bool cas(std::string_view key, std::string_view token, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinCas>(key.size() + token.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putToken(token.data(), u32(token.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    throw_if_error();
    return reply_is<goblin_sbe::StatusReply>();
  }
  [[nodiscard]] std::vector<std::pair<std::string, double>> td_leaderboard_rescore(
      std::string_view key, double now, double hl, long long k, std::uint8_t mode, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinTdRescore>(key.size());
    m.now(now).hl(hl).k(k).mode(mode);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_scored_array();
  }
  [[nodiscard]] long long increx(std::string_view key, long long seconds, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinIncrEx>(key.size());
    m.seconds(seconds);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long zwindow(std::string_view key, double now, double window, long long limit, std::string_view member, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinZWindow>(key.size() + member.size());
    m.now(now).window(window).limit(limit);
    m.putKey(key.data(), u32(key.size()));
    m.putMember(member.data(), u32(member.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long incrbound(std::string_view key, long long delta, long long max, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinIncrBound>(key.size());
    m.delta(delta).max(max);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long decrpos(std::string_view key, ms t = kDefaultTimeout) { return key_int<goblin_sbe::GoblinDecrPos>(key, t); }
  [[nodiscard]] long long hcad(std::string_view key, std::string_view field, std::string_view expected, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinHCad>(key.size() + field.size() + expected.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putExpected(expected.data(), u32(expected.size()));
    finish(m, t);
    return as_int();
  }
  [[nodiscard]] long long hsetgt(std::string_view key, std::string_view field, std::string_view value, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinHSetGt>(key.size() + field.size() + value.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    m.putValue(value.data(), u32(value.size()));
    finish(m, t);
    return as_int();
  }
  // CLAIM: "CLAIMED"/the stored result on a bulk reply, nullopt on nil.
  [[nodiscard]] std::optional<std::string> claim(std::string_view claim_key, std::string_view result_key, std::string_view token, long long seconds, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinClaim>(claim_key.size() + result_key.size() + token.size());
    m.seconds(seconds);
    m.putClaimKey(claim_key.data(), u32(claim_key.size()));
    m.putResultKey(result_key.data(), u32(result_key.size()));
    m.putToken(token.data(), u32(token.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }

  // ---- admin -----------------------------------------------------------------
  [[nodiscard]] std::optional<std::vector<std::pair<std::string, std::string>>> memory(std::string_view key, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinMemory>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_map_or_nil();
  }
  [[nodiscard]] std::optional<long long> optimize(std::string_view key, double density = 0.0, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinOptimize>(key.size());
    m.density(density);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int_or_nil();
  }
  [[nodiscard]] std::string save(std::string_view path, bool accel = true, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinSave>(path.size());
    m.accel(accel ? 1 : 0);
    m.putPath(path.data(), u32(path.size()));
    finish(m, t);
    return as_status();
  }
  [[nodiscard]] long long load(std::string_view path, ms t = kDefaultTimeout) {
    auto m = build<goblin_sbe::GoblinLoad>(path.size());
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
    auto m = build<goblin_sbe::Script>(need);
    m.language(static_cast<std::uint8_t>(lang));
    auto& ag = m.argsCount(u16(args.size()));
    for (const auto& a : args) ag.next().putArg(a.data(), u32(a.size()));
    finish(m, t);
    return as_resp_value();
  }

 private:
  explicit BasicSbeClient(Transport&& transport)
      : transport_(std::move(transport)) {
    const std::size_t floor = transport_.buffer_size_hint();
    sendbuf_.resize(std::max(floor, transport_.send_capacity()));
    cqbuf_.reserve(std::max(floor, transport_.receive_capacity()));
  }

  static std::uint32_t u32(std::size_t n) { return static_cast<std::uint32_t>(n); }
  static std::uint16_t u16(std::size_t n) { return static_cast<std::uint16_t>(n); }

  [[nodiscard]] static std::size_t total_bytes(
      std::span<const std::string_view> values) noexcept {
    std::size_t result = 0;
    for (const auto value : values) {
      result += value.size();
    }
    return result;
  }

  static void require_pubsub_count(std::span<const std::string_view> values,
                                   std::string_view operation) {
    if (values.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("SbeClient: " + std::string(operation) +
                                  " accepts at most 65535 names");
    }
  }

  static void require_pubsub_names(std::span<const std::string_view> values,
                                   std::string_view operation) {
    require_pubsub_count(values, operation);
    if (values.empty()) {
      throw std::invalid_argument("SbeClient: " + std::string(operation) +
                                  " requires at least one name");
    }
  }

  // Wrap a stack-local SBE flyweight over sendbuf_. Flyweights are just
  // (buffer, offset, length) — no heap, no type erasure. Returning by value
  // keeps the hot path free of std::any emplace/cast.
  template <class Msg>
  Msg build(std::size_t payload) {
    if (sendbuf_.size() < payload + 512) sendbuf_.resize(payload + 512);
    Msg m;
    m.wrapAndApplyHeader(sendbuf_.data(), kSbeLenPrefix, sendbuf_.size());
    return m;
  }

  // Common request shapes.
  template <class Msg>
  Msg key_group(std::span<const std::string_view> keys) {
    std::size_t need = 0;
    for (const auto& k : keys) need += k.size();
    Msg m = build<Msg>(need);
    auto& g = m.keysCount(u16(keys.size()));
    for (const auto& k : keys) g.next().putKey(k.data(), u32(k.size()));
    return m;
  }
  template <class Msg>
  Msg field_group(std::string_view key, std::span<const std::string_view> fields) {
    std::size_t need = key.size();
    for (const auto& f : fields) need += f.size();
    Msg m = build<Msg>(need);
    auto& g = m.fieldsCount(u16(fields.size()));
    for (const auto& f : fields) g.next().putField(f.data(), u32(f.size()));
    m.putKey(key.data(), u32(key.size()));
    return m;
  }
  template <class Msg>
  Msg set_members_msg(std::string_view key,
                      std::span<const std::string_view> members) {
    std::size_t need = key.size();
    for (const auto& mbr : members) need += mbr.size();
    Msg m = build<Msg>(need);
    auto& g = m.membersCount(u16(members.size()));
    for (const auto& mbr : members) {
      g.next().putMember(mbr.data(), u32(mbr.size()));
    }
    m.putKey(key.data(), u32(key.size()));
    return m;
  }
  template <class Msg>
  long long set_members_int(std::string_view key,
                            std::span<const std::string_view> members, ms t) {
    auto m = set_members_msg<Msg>(key, members);
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  std::vector<std::string> set_keys_array(std::span<const std::string_view> keys,
                                          ms t) {
    auto m = key_group<Msg>(keys);
    finish(m, t);
    return as_array();
  }
  template <class Msg>
  long long set_store_int(std::string_view destination,
                          std::span<const std::string_view> keys, ms t) {
    std::size_t need = destination.size();
    for (const auto key : keys) need += key.size();
    auto m = build<Msg>(need);
    auto& g = m.keysCount(u16(keys.size()));
    for (const auto key : keys) {
      g.next().putKey(key.data(), u32(key.size()));
    }
    m.putDestination(destination.data(), u32(destination.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  Msg key_member(std::string_view key, std::string_view member) {
    Msg m = build<Msg>(key.size() + member.size());
    m.putKey(key.data(), u32(key.size()));
    m.putMember(member.data(), u32(member.size()));
    return m;
  }
  template <class Msg>
  Msg key_field(std::string_view key, std::string_view field) {
    Msg m = build<Msg>(key.size() + field.size());
    m.putKey(key.data(), u32(key.size()));
    m.putField(field.data(), u32(field.size()));
    return m;
  }
  template <class Msg>
  long long key_int(std::string_view key, ms t) {
    Msg m = build<Msg>(key.size());
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  long long delta_key_int(long long delta, std::string_view key, ms t) {
    Msg m = build<Msg>(key.size());
    m.delta(delta);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  long long list_push(std::string_view key,
                      std::span<const std::string_view> values,
                      SbeListImplementation implementation,
                      bool only_if_exists, ms t) {
    if (values.empty() || values.size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::invalid_argument("SbeClient: list push requires 1..65535 values");
    }
    std::size_t need = key.size();
    for (const auto value : values) need += value.size();
    auto m = build<Msg>(need);
    m.implementation(static_cast<std::uint8_t>(implementation));
    m.onlyIfExists(only_if_exists ? 1 : 0);
    auto& group = m.valuesCount(u16(values.size()));
    for (const auto value : values) {
      group.next().putValue(value.data(), u32(value.size()));
    }
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  template <class Msg>
  std::optional<std::string> list_pop_one(std::string_view key, ms t) {
    auto m = build<Msg>(key.size());
    m.count(-1);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_bulk_or_nil();
  }
  template <class Msg>
  std::optional<std::vector<std::string>> list_pop_many(
      std::string_view key, std::size_t count, ms t) {
    if (count > static_cast<std::size_t>(std::numeric_limits<long long>::max())) {
      throw std::invalid_argument("SbeClient: list pop count is out of range");
    }
    auto m = build<Msg>(key.size());
    m.count(static_cast<long long>(count));
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_array_or_nil();
  }
  template <class Msg>
  long long expire_impl(std::string_view key, long long amount, std::uint8_t flags, ms t) {
    Msg m = build<Msg>(key.size());
    m.amount(amount).flags(flags);
    m.putKey(key.data(), u32(key.size()));
    finish(m, t);
    return as_int();
  }
  goblin_sbe::ZRange zrange_build(std::string_view key, long long start, long long stop, bool ws, bool rev) {
    auto m = build<goblin_sbe::ZRange>(key.size());
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
    Msg m = build<Msg>(need);
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

  void require_no_pipeline() const {
    if (outstanding_pipeline_replies_ != 0) {
      throw std::logic_error(
          "SbeClient: synchronous command would consume a pipelined reply");
    }
  }

  void compact_cq_buffer_for_append() {
    if (cqbuf_offset_ == 0) {
      return;
    }
    if (cqbuf_offset_ == cqbuf_.size()) {
      cqbuf_.clear();
      cqbuf_offset_ = 0;
      return;
    }
    if (cqbuf_offset_ >= cqbuf_.size() / 2) {
      cqbuf_.erase(0, cqbuf_offset_);
      cqbuf_offset_ = 0;
    }
  }

  // Pull every currently published completion record into the client accumulator.
  // This is called from the SQ-full callback so a deep pipeline cannot deadlock
  // with the server blocked on a full CQ.
  bool drain_ready_records(std::exception_ptr& error) noexcept {
    try {
      compact_cq_buffer_for_append();
      while (auto record = transport_.peek()) {
        cqbuf_.append(*record);
        transport_.pop();
      }
      return true;
    } catch (...) {
      error = std::current_exception();
      return false;
    }
  }

  void send_message(std::string_view bytes, ms timeout) {
    const std::size_t maximum = transport_.max_message_bytes();
    if (bytes.size() > maximum) {
      throw std::length_error(
          "SbeClient: message (" + std::to_string(bytes.size()) +
          " bytes) exceeds this transport's message capacity (" +
          std::to_string(maximum) + " bytes)");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::exception_ptr drain_error;
    const bool sent = transport_.send(bytes, [&]() noexcept {
      if (!drain_ready_records(drain_error)) {
        return true;
      }
      return std::chrono::steady_clock::now() >= deadline;
    });
    if (drain_error) {
      std::rethrow_exception(drain_error);
    }
    if (!sent) {
      throw std::runtime_error(
          "SbeClient: timed out or transport failed while enqueueing a message");
    }
  }

  template <class Msg>
  void send_built(Msg& message, ms timeout) {
    const std::size_t message_length =
        goblin_sbe::MessageHeader::encodedLength() + message.encodedLength();
    if (message_length > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("SbeClient: SBE message exceeds uint32 framing");
    }
    const auto length = static_cast<std::uint32_t>(message_length);
    std::memcpy(sendbuf_.data(), &length, kSbeLenPrefix);
    send_message(std::string_view(sendbuf_.data(), kSbeLenPrefix + length),
                 timeout);
  }

  template <class Msg>
  void enqueue_built(Msg& message, ms timeout) {
    send_built(message, timeout);
    ++outstanding_pipeline_replies_;
  }

  // Frame the just-built message and block for its reply.
  template <class Msg>
  void finish(Msg& msg, ms timeout) {
    require_no_pipeline();
    send_built(msg, timeout);
    read_frame(timeout);
  }

  goblin_sbe::PubSub build_pubsub(
      std::uint8_t operation, std::span<const std::string_view> args) {
    require_pubsub_count(args, "pubsub");
    auto message = build<goblin_sbe::PubSub>(total_bytes(args));
    message.operation(operation);
    auto& group = message.argsCount(u16(args.size()));
    for (const auto arg : args) {
      group.next().putArg(arg.data(), u32(arg.size()));
    }
    return message;
  }

  template <class Msg>
  [[nodiscard]] std::vector<PubSubMessage> finish_pubsub(
      Msg& message, std::size_t acknowledgements, ms timeout) {
    require_no_pipeline();
    send_built(message, timeout);

    std::vector<PubSubMessage> result;
    result.reserve(acknowledgements);
    while (result.size() < acknowledgements) {
      read_frame(timeout, true);
      throw_if_error();
      if (!reply_is<goblin_sbe::PubSubPush>()) unexpected();
      auto push = decode_pubsub();
      if (push.kind == PubSubKind::message ||
          push.kind == PubSubKind::pattern_message) {
        pending_pubsub_.push_back(std::move(push));
        continue;
      }
      apply_subscription_ack(push);
      result.push_back(std::move(push));
    }
    return result;
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

  [[nodiscard]] PubSubMessage decode_pubsub() {
    auto reply = decode<goblin_sbe::PubSubPush>();
    PubSubMessage result;
    result.kind = static_cast<PubSubKind>(reply.kind());
    result.subscription_count = reply.subscriptionCount();
    const auto pattern = reply.getPatternAsStringView();
    const auto channel = reply.getChannelAsStringView();
    const auto payload = reply.getPayloadAsStringView();
    result.pattern.assign(pattern.data(), pattern.size());
    result.channel.assign(channel.data(), channel.size());
    result.payload.assign(payload.data(), payload.size());
    return result;
  }

  void apply_subscription_ack(const PubSubMessage& message) noexcept {
    const std::size_t old_total = literal_subscriptions_ + pattern_subscriptions_;
    const std::size_t new_total = message.subscription_count;
    switch (message.kind) {
      case PubSubKind::subscribe:
        if (new_total > old_total) literal_subscriptions_ += new_total - old_total;
        break;
      case PubSubKind::pattern_subscribe:
        if (new_total > old_total) pattern_subscriptions_ += new_total - old_total;
        break;
      case PubSubKind::unsubscribe:
        if (old_total > new_total) {
          literal_subscriptions_ -=
              std::min(literal_subscriptions_, old_total - new_total);
        }
        break;
      case PubSubKind::pattern_unsubscribe:
        if (old_total > new_total) {
          pattern_subscriptions_ -=
              std::min(pattern_subscriptions_, old_total - new_total);
        }
        break;
      case PubSubKind::message:
      case PubSubKind::pattern_message:
        break;
    }
  }
  void throw_if_error() {
    if (reply_is<goblin_sbe::ErrorReply>()) {
      auto e = decode<goblin_sbe::ErrorReply>();
      std::string code(e.getCodeAsStringView());
      std::string message(e.getMessageAsStringView());
      throw std::runtime_error(code + " " + message);
    }
  }
  [[noreturn]] void unexpected() { throw std::runtime_error("SbeClient: unexpected reply type"); }

  [[nodiscard]] long long as_int() {
    // One header read: error check + type check + decode (was 3× reply_header).
    const auto h = reply_header();
    const auto tid = h.templateId();
    if (tid == goblin_sbe::ErrorReply::sbeTemplateId()) {
      auto e = decode<goblin_sbe::ErrorReply>();
      throw std::runtime_error(std::string(e.getCodeAsStringView()) + " " +
                               std::string(e.getMessageAsStringView()));
    }
    if (tid != goblin_sbe::IntReply::sbeTemplateId()) unexpected();
    goblin_sbe::IntReply m;
    m.wrapForDecode(last_frame_.data(), goblin_sbe::MessageHeader::encodedLength(),
                    h.blockLength(), h.version(), last_frame_.size());
    return m.value();
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
  [[nodiscard]] std::optional<std::vector<std::string>> as_array_or_nil() {
    throw_if_error();
    if (reply_is<goblin_sbe::NilReply>()) return std::nullopt;
    return as_array();
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

  [[nodiscard]] bool extract_buffered_frame() {
    const std::size_t available = cqbuf_.size() - cqbuf_offset_;
    if (available < kSbeLenPrefix) {
      return false;
    }
    std::uint32_t length = 0;
    std::memcpy(&length, cqbuf_.data() + cqbuf_offset_, kSbeLenPrefix);
    const std::size_t frame = kSbeLenPrefix + static_cast<std::size_t>(length);
    if (available < frame) {
      return false;
    }
    last_frame_.assign(cqbuf_.data() + cqbuf_offset_ + kSbeLenPrefix, length);
    cqbuf_offset_ += frame;
    if (cqbuf_offset_ == cqbuf_.size()) {
      cqbuf_.clear();
      cqbuf_offset_ = 0;
    }
    return true;
  }

  void read_frame_until(std::chrono::steady_clock::time_point deadline) {
    // Amortize the deadline clock read: a syscall/VDSO every spin dominates the
    // empty-CQ wait (the p99 path). Check every 64 pauses; 5 s timeouts stay exact
    // enough (64 * ~30 ns pause << 1 ms of slack).
    unsigned spins = 0;
    for (;;) {
      if (extract_buffered_frame()) {
        return;
      }
      if (const auto rec = transport_.peek()) {
        // Hot path: empty accumulator + one CQ record holds a whole frame.
        // Copy once into last_frame_ and skip the cqbuf_ append/erase round-trip
        // (two memcpys + string bookkeeping per reply).
        if (cqbuf_.empty()) {
          const auto& bytes = *rec;
          if (bytes.size() >= kSbeLenPrefix) {
            std::uint32_t len = 0;
            std::memcpy(&len, bytes.data(), kSbeLenPrefix);
            const std::size_t frame = kSbeLenPrefix + static_cast<std::size_t>(len);
            if (bytes.size() == frame) {
              last_frame_.assign(bytes.data() + kSbeLenPrefix, len);
              transport_.pop();
              return;
            }
            if (bytes.size() > frame) {
              // Record holds a full frame plus pipelined leftover.
              last_frame_.assign(bytes.data() + kSbeLenPrefix, len);
              cqbuf_.assign(bytes.data() + frame, bytes.size() - frame);
              cqbuf_offset_ = 0;
              transport_.pop();
              return;
            }
          }
        }
        compact_cq_buffer_for_append();
        cqbuf_.append(*rec);
        transport_.pop();
      }
      if ((++spins & 63u) == 0 && std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("SbeClient: timed out waiting for a reply");
      // Adaptive spin-then-park on the CQ tail (macOS); pure relax elsewhere.
      transport_.wait_for_record();
    }
  }

  void read_frame(ms timeout, bool accept_pubsub = false) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      read_frame_until(deadline);
      if (accept_pubsub || !reply_is<goblin_sbe::PubSubPush>()) {
        return;
      }
      auto push = decode_pubsub();
      apply_subscription_ack(push);
      pending_pubsub_.push_back(std::move(push));
    }
  }

  Transport transport_;
  std::vector<char> sendbuf_;  // growable request buffer
  std::string cqbuf_;          // CQ byte accumulator
  std::size_t cqbuf_offset_{0};
  std::string last_frame_;     // the last reply's SBE message (prefix stripped)
  std::deque<PubSubMessage> pending_pubsub_;
  std::size_t literal_subscriptions_{0};
  std::size_t pattern_subscriptions_{0};
  std::size_t outstanding_pipeline_replies_{0};
};

using SbeRingClient = BasicSbeClient<RingSbeTransport>;
using SbeSocketClient = BasicSbeClient<SocketSbeTransport>;
#if defined(GOBLIN_HAS_RDMA)
using SbeRdmaClient = BasicSbeClient<rdma::ClientTransport>;
#endif
#if defined(GOBLIN_HAS_EXASOCK)
using SbeExasockClient = BasicSbeClient<exasock::ClientTransport>;
#endif

}  // namespace goblin::core
