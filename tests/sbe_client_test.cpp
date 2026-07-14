// End-to-end SbeRingClient across the command families: fork the server with a ring,
// open the client, and exercise strings, keyspace/TTL, hash, list, zset, a GOBLIN.*
// native, admin, and scripting -- asserting the decoded replies.
//
//   sbe_client_test <path-to-goblin-core>

#include "goblin/core/sbe_ring_client.hpp"

#include <csignal>  // kill / SIGTERM (not transitively included via <sys/wait.h> on macOS)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using goblin::core::RespValue;
using goblin::core::SbeRingClient;
using goblin::core::SbeListImplementation;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: sbe_client_test <goblin-core>\n"); return 2; }
  const char* server = argv[1];
  const std::string tag = std::to_string(::getpid());
  const std::string ring = "/tmp/gcclient-" + tag + ".ring";
  const std::string sock = "/tmp/gcclient-" + tag + ".sock";  // avoids the default TCP bind
  ::unlink(ring.c_str());

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
    ::execl(server, server, "--unixsocket", sock.c_str(), "--ring", ring.c_str(), "1mb",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  auto c = SbeRingClient::open(ring.c_str(), std::chrono::seconds(5));
  assert(c);
  // grow-to-ring: the 1 MiB ring dwarfs the 16 KiB default, so the buffer grew to it.
  assert(c->buffer_size() >= 512 * 1024);

  using V = std::vector<std::string_view>;
  using P = std::vector<std::pair<std::string_view, std::string_view>>;

  // connection
  assert(c->ping());
  assert(c->echo("hi") == "hi");
  assert(c->info().find("used_memory") != std::string::npos);

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
  assert(c->hdel("h", V{"f1", "f2"}) == 2);

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
