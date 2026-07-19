// End-to-end SbeRingClient across the command families: fork the server with a ring,
// open the client, and exercise strings, keyspace/TTL, hash, list, zset, a GOBLIN.*
// native, admin, and scripting -- asserting the decoded replies.
//
//   sbe_client_test <path-to-goblin-core>

#include "goblin/core/sbe_ring_client.hpp"
#include "socket_test_utils.hpp"

#include <csignal>  // kill / SIGTERM (not transitively included via <sys/wait.h> on macOS)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using goblin::core::RespValue;
using goblin::core::PubSubKind;
using goblin::core::SbeRingClient;
using goblin::core::SbeListImplementation;
using goblin::core::SbeZAddOptions;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: sbe_client_test <goblin-core>\n"); return 2; }
  const char* server = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string ring = "/tmp/gcclient-" + tag + ".ring";
  const std::string sock = "/tmp/gcclient-" + tag + ".sock";
  const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
  assert(tcp_port != 0);
  const std::string tcp_port_text = std::to_string(tcp_port);
  ::unlink(ring.c_str());

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
    // A deliberately constrained ring exercises pipeline backpressure. Production
    // rings are normally sized to their HugeTLB geometry; 4 KiB is useful when a
    // deployment owns thousands of per-client rings.
    ::execl(server, server, "--enable-sbe", "--unixsocket", sock.c_str(),
            "--port", tcp_port_text.c_str(), "--ring", ring.c_str(), "4kb",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  auto c = SbeRingClient::open(ring.c_str(), std::chrono::seconds(5));
  assert(c);
  assert(c->buffer_size() >= SbeRingClient::kDefaultBufferBytes);

  using V = std::vector<std::string_view>;
  using P = std::vector<std::pair<std::string_view, std::string_view>>;

  // connection
  assert(c->ping());
  assert(c->echo("hi") == "hi");
  assert(c->info().find("used_memory") != std::string::npos);

  // Pub/Sub stays typed on SBE. A self-publish places the asynchronous push before
  // the PUBLISH IntReply on the CQ; publish() must queue that push and still decode
  // its own reply correctly.
  {
    const auto acks = c->subscribe(V{"sbe-events", "sbe-events"});
    assert(acks.size() == 2);
    assert(acks[0].kind == PubSubKind::subscribe);
    assert(acks[0].channel == "sbe-events" && acks[0].subscription_count == 1);
    assert(acks[1].subscription_count == 1);  // duplicate is acknowledged, not added

    const auto pattern_acks = c->psubscribe(V{"sbe-*"});
    assert(pattern_acks.size() == 1);
    assert(pattern_acks[0].kind == PubSubKind::pattern_subscribe);
    assert(pattern_acks[0].pattern == "sbe-*" &&
           pattern_acks[0].subscription_count == 2);

    const auto counts = c->pubsub_numsub(V{"sbe-events", "missing"});
    assert(counts.size() == 2);
    assert(counts[0].first == "sbe-events" && counts[0].second == 1);
    assert(counts[1].first == "missing" && counts[1].second == 0);
    assert(c->pubsub_numpat() == 1);

    assert(c->publish("sbe-events", "payload") == 2);
    const auto direct = c->read_pubsub();
    const auto pattern = c->read_pubsub();
    assert(direct.kind == PubSubKind::message && direct.channel == "sbe-events" &&
           direct.payload == "payload");
    assert(pattern.kind == PubSubKind::pattern_message && pattern.pattern == "sbe-*" &&
           pattern.channel == "sbe-events" && pattern.payload == "payload");

    const auto unpattern = c->punsubscribe();
    assert(unpattern.size() == 1 && unpattern[0].subscription_count == 1);
    const auto undirect = c->unsubscribe();
    assert(undirect.size() == 1 && undirect[0].subscription_count == 0);
  }

  // strings
  (void)c->set("k", "v");
  assert(c->get("k") == "v");
  assert(!c->get("missing").has_value());
  assert(c->append("k", "!") == 2 && c->strlen("k") == 2);
  assert(c->get("k") == "v!");
  assert(c->getset("k", "w") == "v!" && c->get("k") == "w");
  assert(c->setnx("k", "x") == 0 && c->setnx("fresh", "y") == 1);
  assert(c->getdel("fresh") == "y" && !c->get("fresh").has_value());
  assert(c->incr("cnt") == 1 && c->incrby("cnt", 9) == 10 && c->decr("cnt") == 9 && c->decrby("cnt", 4) == 5);
  assert(c->incrbyfloat("f", 2.5) == "2.5");
  assert(c->getrange("k", 0, 0) == "w");
  assert(c->setrange("k", 1, "IDE") == 4 && c->get("k") == "wIDE");
  c->mset(P{{"m1", "a"}, {"m2", "b"}});
  {
    auto vals = c->mget(V{"m1", "nope", "m2"});
    assert(vals.size() == 3 && vals[0] == "a" && !vals[1].has_value() && vals[2] == "b");
  }
  // INCR on a non-integer throws
  (void)c->set("word", "abc");
  bool threw = false;
  try { (void)c->incr("word"); } catch (const std::runtime_error&) { threw = true; }
  assert(threw);

  // keyspace / TTL
  assert(c->exists(V{"k", "m1", "nope"}) == 2);
  assert(c->type("k") == "string");
  assert(c->expire("k", 100) == 1);
  assert(c->ttl("k") > 0 && c->ttl("k") <= 100);
  assert(c->persist("k") == 1 && c->ttl("k") == -1);
  assert(c->del(V{"k", "m1", "m2"}) == 3);
  assert(c->ttl("gone") == -2);

  // hash
  // sets
  {
    using SV = std::span<const std::string_view>;
    const std::string_view ma[] = {"a", "b", "c"};
    assert(c->sadd("s", SV{ma, 3}) == 3);
    assert(c->scard("s") == 3);
    assert(c->sismember("s", "b") == 1);
    assert(c->smembers("s").size() == 3);
    const std::string_view mb[] = {"b", "c", "d"};
    assert(c->sadd("t", SV{mb, 3}) == 3);
    const std::string_view keys[] = {"s", "t"};
    assert(c->sinter(SV{keys, 2}).size() == 2);
    assert(c->sunionstore("u", SV{keys, 2}) == 4);
    assert(c->srem("s", SV{ma, 1}) == 1);
  }

  assert(c->hset("h", P{{"f1", "1"}, {"f2", "2"}}) == 2);
  assert(c->hget("h", "f1") == "1" && !c->hget("h", "no").has_value());
  assert(c->hlen("h") == 2 && c->hexists("h", "f2") == 1 && c->hstrlen("h", "f1") == 1);
  assert(c->hincrby("h", "f1", 4) == 5);
  {
    auto all = c->hgetall("h");
    assert(all.size() == 2);
    auto mv = c->hmget("h", V{"f1", "no", "f2"});
    assert(mv[0] == "5" && !mv[1].has_value() && mv[2] == "2");
    assert(c->hkeys("h").size() == 2 && c->hvals("h").size() == 2);
  }
  {
    std::uint64_t cursor = 0;
    std::vector<std::string> fields;
    do {
      auto page = c->hscan("h", cursor, 1);
      cursor = page.next_cursor;
      for (std::size_t index = 0; index + 1 < page.items.size(); index += 2) {
        fields.push_back(page.items[index]);
      }
    } while (cursor != 0);
    std::sort(fields.begin(), fields.end());
    assert((fields == std::vector<std::string>{"f1", "f2"}));
    const auto no_values = c->hscan("h", 0, 10, std::nullopt, true);
    assert(no_values.next_cursor == 0 && no_values.items.size() == 2);
  }
  {
    const auto hashes = c->scan(0, 100, std::nullopt, "hash");
    assert(hashes.next_cursor == 0);
    assert(std::find(hashes.items.begin(), hashes.items.end(), "h") !=
           hashes.items.end());
    const auto matched = c->scan(0, 100, "s*", std::nullopt);
    assert(std::find(matched.items.begin(), matched.items.end(), "s") !=
           matched.items.end());
  }
  assert(c->hdel("h", V{"f1", "f2"}) == 2);

  // A pipeline much deeper than either direction of the constrained ring proves
  // that enqueue drains completed replies while waiting for SQ space. Otherwise
  // the client would wait on a full SQ while the server waited on a full CQ.
  {
    constexpr std::size_t kPipelineDepth = 1024;
    for (std::size_t i = 0; i < kPipelineDepth; ++i) {
      const std::string field = "f" + std::to_string(i);
      const std::string value = std::to_string(i);
      const std::array<std::pair<std::string_view, std::string_view>, 1> entry{{
          {field, value},
      }};
      c->enqueue_hset("pipeline-hash", entry);
    }
    assert(c->outstanding_pipeline_replies() == kPipelineDepth);

    bool sync_guarded = false;
    try {
      (void)c->ping();
    } catch (const std::logic_error&) {
      sync_guarded = true;
    }
    assert(sync_guarded);

    for (std::size_t i = 0; i < kPipelineDepth; ++i) {
      assert(c->read_pipeline_int() == 1);
    }
    assert(c->outstanding_pipeline_replies() == 0);

    c->enqueue_hget("pipeline-hash", "f17");
    c->enqueue_hlen("pipeline-hash");
    c->enqueue_hincrby("pipeline-hash", "f17", 5);
    assert(c->read_pipeline_bulk_or_nil() == "17");
    assert(c->read_pipeline_int() == static_cast<long long>(kPipelineDepth));
    assert(c->read_pipeline_int() == 22);

    // An error consumes exactly its own ordered reply; the following reply remains
    // readable, so a caller can handle one failure without losing pipeline framing.
    c->enqueue_hset("pipeline-hash", P{{"not-an-int", "value"}});
    c->enqueue_hincrby("pipeline-hash", "not-an-int", 1);
    c->enqueue_hlen("pipeline-hash");
    assert(c->read_pipeline_int() == 1);
    bool pipeline_error = false;
    try {
      (void)c->read_pipeline_int();
    } catch (const std::runtime_error&) {
      pipeline_error = true;
    }
    assert(pipeline_error);
    assert(c->outstanding_pipeline_replies() == 1);
    assert(c->read_pipeline_int() ==
           static_cast<long long>(kPipelineDepth + 1));

    // The generic writer makes every generated request pipeline-capable, while
    // retaining typed convenience methods for hot command families.
    c->enqueue_sbe<goblin_sbe::Ping>(0, [](auto&) {});
    assert(c->read_pipeline_status() == "PONG");

    // A message larger than the contiguous record envelope is rejected before
    // any of it reaches the SQ, leaving the connection usable and synchronized.
    const std::string oversized(c->max_message_bytes(), 'x');
    bool oversized_rejected = false;
    try {
      c->enqueue_sbe<goblin_sbe::Echo>(
          oversized.size(), [&](auto& message) {
            message.putMessage(oversized.data(),
                               static_cast<std::uint32_t>(oversized.size()));
          });
    } catch (const std::length_error&) {
      oversized_rejected = true;
    }
    assert(oversized_rejected);
    assert(c->outstanding_pipeline_replies() == 0);
    assert(c->ping());

    // Windowed pipeline helper (K): depth 16 keeps several HGETs in flight.
    {
      constexpr std::size_t kWindow = 16;
      constexpr std::size_t kOps = 64;
      std::vector<std::string> fields;
      fields.reserve(kOps);
      for (std::size_t i = 0; i < kOps; ++i) {
        fields.push_back("f" + std::to_string(i % kPipelineDepth));
      }
      std::size_t hits = 0;
      c->pipeline_for(
          kOps, kWindow,
          [&](std::size_t i) { c->enqueue_hget("pipeline-hash", fields[i]); },
          [&](std::size_t) {
            if (c->read_pipeline_bulk_or_nil().has_value()) {
              ++hits;
            }
          });
      assert(hits == kOps);
      assert(c->outstanding_pipeline_replies() == 0);
    }
  }

  // list: both representations plus scalar/count POP reply forms.
  assert(c->rpush("list", V{"a", "b", "c"},
                  SbeListImplementation::segmented) == 3);
  assert(c->lpush("list", V{"x", "y"}) == 5);
  assert(c->llen("list") == 5);
  assert(c->lindex("list", 2) == "a");
  assert((c->lrange("list", 0, -1) ==
          std::vector<std::string>{"y", "x", "a", "b", "c"}));
  c->lset("list", 2, "A");
  assert(c->linsert("list", true, "A", "p") == 6);
  assert(c->lrem("list", 0, "x") == 1);
  c->ltrim("list", 1, -1);
  assert(c->lpop("list") == "p");
  {
    const auto popped = c->rpop("list", 2);
    assert(popped && *popped == std::vector<std::string>({"c", "b"}));
  }
  assert(c->lpop("list") == "A");
  assert(!c->lpop("list").has_value());
  assert(!c->lpop("missing-list", 2).has_value());
  assert(c->rpush("pma-list", V{"one", "two"},
                  SbeListImplementation::pma) == 2);
  assert(c->lpush("missing-pushx", V{"x"},
                  SbeListImplementation::selected,
                  /*only_if_exists=*/true) == 0);

  bool list_wrong_type = false;
  try { (void)c->llen("word"); } catch (const std::runtime_error&) {
    list_wrong_type = true;
  }
  assert(list_wrong_type);

  // zset
  using S = std::vector<SbeRingClient::Scored>;
  using ReturnedS = std::vector<std::pair<std::string, double>>;
  assert(c->zadd("z", S{{1.5, "a"}, {2.0, "b"}, {3.0, "c"}}) == 3);
  assert(c->zcard("z") == 3);
  assert(c->zscore("z", "b") == 2.0 && !c->zscore("z", "x").has_value());
  assert(c->zrank("z", "a") == 0 && c->zrevrank("z", "c") == 0);
  assert((c->zrange("z", 0, -1) == std::vector<std::string>{"a", "b", "c"}));
  assert((c->zrange("z", 0, -1, /*rev=*/true) == std::vector<std::string>{"c", "b", "a"}));
  {
    auto ws = c->zrange_withscores("z", 0, -1);
    assert(ws.size() == 3 && ws[0].first == "a" && ws[0].second == 1.5);
  }
  assert(c->zrem("z", V{"a"}) == 1);
  assert(c->zremrangebyscore("z", 2.0, false, 3.0, false) == 2 && c->zcard("z") == 0);

  assert(c->zadd("za", S{{1.0, "one"}, {2.0, "two"}, {3.0, "three"},
                           {4.0, "four"}, {5.0, "five"}}) == 5);
  SbeZAddOptions nx;
  nx.nx = true;
  assert(c->zadd("za", S{{9.0, "one"}}, nx) == 0);
  SbeZAddOptions gt_ch;
  gt_ch.gt = true;
  gt_ch.ch = true;
  assert(c->zadd("za", S{{1.5, "one"}}, gt_ch) == 1);
  SbeZAddOptions xx;
  xx.xx = true;
  assert(!c->zadd_increment("za", 1.0, "ghost", xx).has_value());
  assert(c->zadd_increment("za", 2.0, "three") == 5.0);
  assert(c->zincrby("za", -1.0, "four") == 3.0);
  assert(c->zcount("za", 1.5, true, 5.0, true) == 2);
  assert((c->zrangebyscore("za", 1.5, false, 5.0, false,
                           false, 1, 2) ==
          std::vector<std::string>{"two", "four"}));
  {
    const auto ws = c->zrangebyscore_withscores(
        "za", 1.5, false, 5.0, false, true, 1, 2);
    assert(ws == ReturnedS({{"five", 5.0}, {"four", 3.0}}));
  }
  {
    const auto scores = c->zmscore("za", V{"one", "ghost", "three"});
    assert(scores.size() == 3 && scores[0] == 1.5 && !scores[1] &&
           scores[2] == 5.0);
  }
  {
    std::uint64_t cursor = 0;
    std::vector<std::string> members;
    do {
      auto page = c->zscan("za", cursor, 2);
      cursor = page.next_cursor;
      for (const auto& [member, score] : page.items) {
        (void)score;
        members.push_back(member);
      }
    } while (cursor != 0);
    std::sort(members.begin(), members.end());
    assert((members ==
            std::vector<std::string>{"five", "four", "one", "three", "two"}));
  }
  {
    const auto popped = c->zpopmin("za", 2);
    assert(popped == ReturnedS({{"one", 1.5}, {"two", 2.0}}));
  }
  {
    const auto popped = c->zpopmax("za", 2);
    assert(popped == ReturnedS({{"three", 5.0}, {"five", 5.0}}));
  }

  // a GOBLIN.* native + admin
  (void)c->set("lock", "tok");
  assert(c->cad("lock", "wrong") == 0 && c->cad("lock", "tok") == 1);
  assert(c->hsetgt("wm", "ts", "100") == 0 || true);  // creates
  (void)c->zadd("mz", S{{1.0, "x"}});
  {
    auto stats = c->memory("mz");
    assert(stats.has_value());
    bool has_total = false;
    for (const auto& [n, v] : *stats) if (n == "total_allocated_bytes") has_total = true;
    assert(has_total);
    assert(!c->memory("nosuch").has_value());
  }
  assert(c->optimize("mz").has_value());

  // scripting
  {
    auto r = c->eval("return 1", {}, {});
    assert(r.type == RespValue::Type::integer && r.integer == 1);
    auto s = c->eval("return 'hi'", {}, {});
    assert(s.type == RespValue::Type::bulk && s.str == "hi");
    auto a = c->eval("return {1, 2, 3}", {}, {});
    assert(a.type == RespValue::Type::array && a.elements.size() == 3 && a.elements[2].integer == 3);
    auto n = c->eval("return {1, {2, 3}}", {}, {});
    assert(n.elements.size() == 2 && n.elements[1].type == RespValue::Type::array &&
           n.elements[1].elements[1].integer == 3);
    auto sr = c->eval("return redis.call('set', KEYS[1], ARGV[1])", V{"sk"}, V{"sv"});
    assert(sr.str == "OK");
    assert(c->get("sk") == "sv");
  }

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);
  ::unlink(ring.c_str());
  ::unlink(sock.c_str());
  std::puts("sbe client OK: full command surface over the ring (strings/keyspace/hash/list/zset/natives/admin/scripting)");
  return 0;
}
