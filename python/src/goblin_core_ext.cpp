// nanobind extension backing the goblin_core Python package: a redis-py-shaped
// client that speaks the SBE binary wire to a goblin-core server over a shared-memory
// ring buffer.
//
// It wraps the C++ SbeRingClient. The Python layer (goblin_core/__init__.py) is the
// same redis-py surface as before -- it hands us one already-encoded command as a
// list[bytes] ([b"GET", b"key"]); here we dispatch on the command name to the client's
// typed method (native double/int64 straight onto the wire, no RESP text), then shape
// the reply back into the bytes / int / float / None / list objects redis-py returns.
// An SBE ErrorReply becomes a ResponseError; a transport failure a RingError.
//
// SBE is typed per command, so -- unlike the old generic RESP encoder -- only the
// commands goblin-core actually implements are wired here; that is the whole command
// set the Python layer exposes plus the GOBLIN.* natives and admin verbs.
//
// The GIL is released for the whole ring round trip (build + busy-poll + decode), so a
// spinning poll on one thread never starves other Python threads.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "goblin/core/sbe_ring_client.hpp"
#if defined(GOBLIN_HAS_RDMA)
#include "goblin/core/rdma_ring.hpp"
#endif
#if defined(GOBLIN_HAS_EXASOCK)
#include "goblin/core/exasock_transport.hpp"
#endif

#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;
using goblin::core::Language;
using goblin::core::RespValue;
using goblin::core::SbeRingClient;
#if defined(GOBLIN_HAS_RDMA)
using goblin::core::SbeRdmaClient;
#endif
#if defined(GOBLIN_HAS_EXASOCK)
using goblin::core::SbeExasockClient;
#endif
using goblin::core::SetOptions;
using goblin::core::SetReply;

namespace {

// A command error (an SBE ErrorReply, or a client-side arity/usage error) -- the same
// shape redis-py raises. A transport failure (ring cannot open, reply timed out) is a
// RingError.
struct ResponseError : std::runtime_error { using std::runtime_error::runtime_error; };
struct RingError : std::runtime_error { using std::runtime_error::runtime_error; };

using ms = std::chrono::milliseconds;

// ---- arg parsing (string_views point into the caller's Python bytes) ----------
[[nodiscard]] long long to_ll(std::string_view s) {
  long long v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}
[[nodiscard]] double to_d(std::string_view s) {
  if (s == "+inf" || s == "inf") return HUGE_VAL;
  if (s == "-inf") return -HUGE_VAL;
  double v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}
// A zset score bound: a leading '(' means exclusive (ZREMRANGEBYSCORE).
struct Bound { double value; bool exclusive; };
[[nodiscard]] Bound to_bound(std::string_view s) {
  bool excl = false;
  if (!s.empty() && s.front() == '(') { excl = true; s.remove_prefix(1); }
  return {to_d(s), excl};
}
[[nodiscard]] std::string upper(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}
[[nodiscard]] bool is_zadd_flag(std::string_view s) {
  const std::string u = upper(s);
  return u == "NX" || u == "XX" || u == "GT" || u == "LT" || u == "CH" || u == "INCR";
}

// ---- reply shaping (C++ result -> the object redis-py expects) -----------------
[[nodiscard]] nb::object py_bytes(std::string_view s) { return nb::bytes(s.data(), s.size()); }
[[nodiscard]] nb::object ob(const std::optional<std::string>& v) {
  return v ? py_bytes(*v) : nb::none();
}
[[nodiscard]] nb::object ob(std::optional<long long> v) {
  return v ? nb::object(nb::int_(*v)) : nb::none();
}
[[nodiscard]] nb::object ob(std::optional<double> v) {
  return v ? nb::object(nb::float_(*v)) : nb::none();
}
[[nodiscard]] nb::object ob(long long v) { return nb::int_(v); }
[[nodiscard]] nb::object ob_list(const std::vector<std::string>& v) {
  nb::list out;
  for (const auto& s : v) out.append(py_bytes(s));
  return out;
}
[[nodiscard]] nb::object ob_nlist(const std::vector<std::optional<std::string>>& v) {
  nb::list out;
  for (const auto& s : v) out.append(s ? py_bytes(*s) : nb::none());
  return out;
}
// ZRANGE WITHSCORES -> a flat [member, score, member, score, ...] the layer zips.
[[nodiscard]] nb::object ob_flat_scored(const std::vector<std::pair<std::string, double>>& v) {
  nb::list out;
  for (const auto& [m, s] : v) { out.append(py_bytes(m)); out.append(nb::float_(s)); }
  return out;
}
// HGETALL / GOBLIN.MEMORY -> a flat [key, value, ...] (the layer zips it into a dict).
[[nodiscard]] nb::object ob_flat_pairs(const std::vector<std::pair<std::string, std::string>>& v) {
  nb::list out;
  for (const auto& [k, val] : v) { out.append(py_bytes(k)); out.append(py_bytes(val)); }
  return out;
}
// A script result tree -> Python (an error node raises, like a RESP -err reply).
[[nodiscard]] nb::object ob_resp(const RespValue& v) {
  switch (v.type) {
    case RespValue::Type::integer: return nb::int_(v.integer);
    case RespValue::Type::bulk:
    case RespValue::Type::status: return py_bytes(v.str);
    case RespValue::Type::error: throw ResponseError(v.str);
    case RespValue::Type::array: {
      nb::list out;
      for (const auto& e : v.elements) out.append(ob_resp(e));
      return out;
    }
    default: return nb::none();
  }
}

// Run the ring round trip (build + busy-poll + decode) off the GIL, then return its
// C++ result. The arg string_views point into Python bytes held alive by the caller's
// args list, so reading them without the GIL is safe.
template <class F>
auto off_gil(F&& f) -> decltype(f()) {
  nb::gil_scoped_release release;
  return f();
}

// A script command name -> (language, verb: 0 eval / 1 evalsha / 2 script), or none.
// Plain EVAL/EVALSHA/SCRIPT are Lua; a <LANG>. prefix selects another engine.
[[nodiscard]] std::optional<std::pair<Language, int>> script_verb(std::string_view cmd) {
  Language lang = Language::lua;
  std::string_view rest = cmd;
  if (const auto dot = cmd.find('.'); dot != std::string_view::npos) {
    const auto p = cmd.substr(0, dot);
    if (p == "LUAU") lang = Language::luau;
    else if (p == "WREN") lang = Language::wren;
    else if (p == "TCL") lang = Language::tcl;
    else if (p == "UPYTHON") lang = Language::micropython;
    else if (p == "QUICKJS") lang = Language::quickjs;
    else return std::nullopt;  // e.g. GOBLIN.*
    rest = cmd.substr(dot + 1);
  }
  if (rest == "EVAL") return std::pair{lang, 0};
  if (rest == "EVALSHA") return std::pair{lang, 1};
  if (rest == "SCRIPT") return std::pair{lang, 2};
  return std::nullopt;
}

// Shared SBE dispatch for ring and optional ExaSock TCP clients.
template <class SbeClientT>
class BasicPyClient {
 public:
  [[nodiscard]] std::size_t buffer_size() const {
    return client_ ? client_->buffer_size() : 0;
  }

  // One already-encoded command (list[bytes]); returns the shaped reply.
  nb::object execute_command(nb::list args, long timeout_ms) {
    std::vector<std::string_view> a;
    const Py_ssize_t argc = PyList_GET_SIZE(args.ptr());
    a.reserve(static_cast<std::size_t>(argc));
    for (Py_ssize_t i = 0; i < argc; ++i) {
      PyObject* item = PyList_GET_ITEM(args.ptr(), i);
      if (!PyBytes_Check(item)) throw std::invalid_argument("command arguments must be bytes");
      a.emplace_back(PyBytes_AS_STRING(item), static_cast<std::size_t>(PyBytes_GET_SIZE(item)));
    }
    if (a.empty()) throw std::invalid_argument("empty command");

    try {
      return dispatch(upper(a[0]), a, ms(timeout_ms));
    } catch (const ResponseError&) {
      throw;  // already the right Python type
    } catch (const std::runtime_error& e) {
      // BasicSbeClient throws "SbeRingClient: ..." for transport failures and
      // "<code> <message>" for an ErrorReply.
      const std::string msg = e.what();
      if (msg.rfind("SbeRingClient:", 0) == 0) throw RingError(msg);
      throw ResponseError(msg);
    }
  }

 private:
  nb::object dispatch(const std::string& cmd, const std::vector<std::string_view>& a, ms t) {
    SbeClientT& c = *client_;
    using V = std::vector<std::string_view>;
    // Bounds-safe indexed access (a malformed execute_command must not read past the end).
    auto at = [&](std::size_t i) -> std::string_view {
      if (i >= a.size()) throw ResponseError("ERR wrong number of arguments for '" + cmd + "'");
      return a[i];
    };

    // ---- scripting (EVAL / EVALSHA / SCRIPT, with an optional language prefix) --
    if (const auto sv = script_verb(cmd)) {
      const auto [lang, verb] = *sv;
      if (verb == 2) {  // SCRIPT subcommand args...
        V sargs(a.begin() + 1, a.end());
        return ob_resp(off_gil([&] { return c.script(sargs, lang, t); }));
      }
      const std::string_view code = at(1);
      const auto nk = static_cast<std::size_t>(to_ll(at(2)));
      V keys, argv;
      for (std::size_t i = 0; i < nk && 3 + i < a.size(); ++i) keys.push_back(a[3 + i]);
      for (std::size_t i = 3 + keys.size(); i < a.size(); ++i) argv.push_back(a[i]);
      if (verb == 0) return ob_resp(off_gil([&] { return c.eval(code, keys, argv, lang, t); }));
      return ob_resp(off_gil([&] { return c.eval_sha(code, keys, argv, lang, t); }));
    }

    // ---- connection ----------------------------------------------------------
    if (cmd == "PING") {
      // The SBE Ping carries no payload; PING <msg> has the same reply as ECHO <msg>,
      // so route it through the real Echo command (a true round trip of the bytes).
      if (a.size() > 1) return py_bytes(off_gil([&] { return c.echo(at(1), t); }));
      return off_gil([&] { return c.ping(t); }) ? py_bytes("PONG") : nb::none();
    }
    if (cmd == "ECHO") return py_bytes(off_gil([&] { return c.echo(at(1), t); }));
    if (cmd == "INFO") return py_bytes(off_gil([&] { return c.info(t); }));

    // ---- strings -------------------------------------------------------------
    if (cmd == "GET") return ob(off_gil([&] { return c.get(at(1), t); }));
    if (cmd == "SET") {
      SetOptions o;
      for (std::size_t i = 3; i < a.size(); ++i) {
        const std::string tok = upper(a[i]);
        if (tok == "NX") o.nx = true;
        else if (tok == "XX") o.xx = true;
        else if (tok == "GET") o.get = true;
        else if (tok == "KEEPTTL") o.keepttl = true;
        else if (tok == "EX") { o.expire_mode = 1; o.expire_value = to_ll(at(++i)); }
        else if (tok == "PX") { o.expire_mode = 2; o.expire_value = to_ll(at(++i)); }
        else if (tok == "EXAT") { o.expire_mode = 3; o.expire_value = to_ll(at(++i)); }
        else if (tok == "PXAT") { o.expire_mode = 4; o.expire_value = to_ll(at(++i)); }
      }
      const SetReply r = off_gil([&] { return c.set(at(1), at(2), o, t); });
      if (r.old) return py_bytes(*r.old);          // SET ... GET: the prior value
      return r.ok ? py_bytes("OK") : nb::none();   // +OK, or nil when a condition failed
    }
    if (cmd == "GETSET") return ob(off_gil([&] { return c.getset(at(1), at(2), t); }));
    if (cmd == "SETNX") return ob(off_gil([&] { return c.setnx(at(1), at(2), t); }));
    if (cmd == "GETDEL") return ob(off_gil([&] { return c.getdel(at(1), t); }));
    if (cmd == "STRLEN") return ob(off_gil([&] { return c.strlen(at(1), t); }));
    if (cmd == "APPEND") return ob(off_gil([&] { return c.append(at(1), at(2), t); }));
    if (cmd == "INCR") return ob(off_gil([&] { return c.incr(at(1), t); }));
    if (cmd == "DECR") return ob(off_gil([&] { return c.decr(at(1), t); }));
    if (cmd == "INCRBY") return ob(off_gil([&] { return c.incrby(at(1), to_ll(at(2)), t); }));
    if (cmd == "DECRBY") return ob(off_gil([&] { return c.decrby(at(1), to_ll(at(2)), t); }));
    if (cmd == "INCRBYFLOAT") return py_bytes(off_gil([&] { return c.incrbyfloat(at(1), to_d(at(2)), t); }));
    if (cmd == "GETRANGE") return py_bytes(off_gil([&] { return c.getrange(at(1), to_ll(at(2)), to_ll(at(3)), t); }));
    if (cmd == "SETRANGE") return ob(off_gil([&] { return c.setrange(at(1), to_ll(at(2)), at(3), t); }));
    if (cmd == "MSET") {
      std::vector<std::pair<std::string_view, std::string_view>> pairs;
      for (std::size_t i = 1; i + 1 < a.size(); i += 2) pairs.emplace_back(a[i], a[i + 1]);
      off_gil([&] { c.mset(pairs, t); return 0; });
      return py_bytes("OK");
    }
    if (cmd == "MGET") { V keys(a.begin() + 1, a.end()); return ob_nlist(off_gil([&] { return c.mget(keys, t); })); }

    // ---- keyspace / TTL ------------------------------------------------------
    if (cmd == "DEL") { V keys(a.begin() + 1, a.end()); return ob(off_gil([&] { return c.del(keys, t); })); }
    if (cmd == "EXISTS") { V keys(a.begin() + 1, a.end()); return ob(off_gil([&] { return c.exists(keys, t); })); }
    if (cmd == "TYPE") return py_bytes(off_gil([&] { return c.type(at(1), t); }));
    if (cmd == "EXPIRE") return ob(off_gil([&] { return c.expire(at(1), to_ll(at(2)), 0, t); }));
    if (cmd == "PEXPIRE") return ob(off_gil([&] { return c.pexpire(at(1), to_ll(at(2)), 0, t); }));
    if (cmd == "EXPIREAT") return ob(off_gil([&] { return c.expireat(at(1), to_ll(at(2)), 0, t); }));
    if (cmd == "PEXPIREAT") return ob(off_gil([&] { return c.pexpireat(at(1), to_ll(at(2)), 0, t); }));
    if (cmd == "TTL") return ob(off_gil([&] { return c.ttl(at(1), t); }));
    if (cmd == "PTTL") return ob(off_gil([&] { return c.pttl(at(1), t); }));
    if (cmd == "PERSIST") return ob(off_gil([&] { return c.persist(at(1), t); }));
    if (cmd == "EXPIRETIME") return ob(off_gil([&] { return c.expiretime(at(1), t); }));
    if (cmd == "PEXPIRETIME") return ob(off_gil([&] { return c.pexpiretime(at(1), t); }));

    // ---- hash ----------------------------------------------------------------
    if (cmd == "HSET") {
      std::vector<std::pair<std::string_view, std::string_view>> fv;
      for (std::size_t i = 2; i + 1 < a.size(); i += 2) fv.emplace_back(a[i], a[i + 1]);
      return ob(off_gil([&] { return c.hset(at(1), fv, t); }));
    }
    if (cmd == "HSETNX") return ob(off_gil([&] { return c.hsetnx(at(1), at(2), at(3), t); }));
    if (cmd == "HGET") return ob(off_gil([&] { return c.hget(at(1), at(2), t); }));
    if (cmd == "HMGET") { V f(a.begin() + 2, a.end()); return ob_nlist(off_gil([&] { return c.hmget(at(1), f, t); })); }
    if (cmd == "HDEL") { V f(a.begin() + 2, a.end()); return ob(off_gil([&] { return c.hdel(at(1), f, t); })); }
    if (cmd == "HGETALL") return ob_flat_pairs(off_gil([&] { return c.hgetall(at(1), t); }));
    if (cmd == "HKEYS") return ob_list(off_gil([&] { return c.hkeys(at(1), t); }));
    if (cmd == "HVALS") return ob_list(off_gil([&] { return c.hvals(at(1), t); }));
    if (cmd == "HLEN") return ob(off_gil([&] { return c.hlen(at(1), t); }));
    if (cmd == "HEXISTS") return ob(off_gil([&] { return c.hexists(at(1), at(2), t); }));
    if (cmd == "HSTRLEN") return ob(off_gil([&] { return c.hstrlen(at(1), at(2), t); }));
    if (cmd == "HINCRBY") return ob(off_gil([&] { return c.hincrby(at(1), at(2), to_ll(at(3)), t); }));

    // ---- sets ----------------------------------------------------------------
    if (cmd == "SADD") {
      V m(a.begin() + 2, a.end());
      return ob(off_gil([&] { return c.sadd(at(1), m, t); }));
    }
    if (cmd == "SREM") {
      V m(a.begin() + 2, a.end());
      return ob(off_gil([&] { return c.srem(at(1), m, t); }));
    }
    if (cmd == "SCARD") return ob(off_gil([&] { return c.scard(at(1), t); }));
    if (cmd == "SISMEMBER")
      return ob(off_gil([&] { return c.sismember(at(1), at(2), t); }));
    if (cmd == "SMISMEMBER") {
      V m(a.begin() + 2, a.end());
      return ob_list(off_gil([&] { return c.smismember(at(1), m, t); }));
    }
    if (cmd == "SMEMBERS")
      return ob_list(off_gil([&] { return c.smembers(at(1), t); }));
    if (cmd == "SPOP") {
      if (a.size() == 2)
        return ob(off_gil([&] { return c.spop(at(1), t); }));
      return ob_list(off_gil(
          [&] { return c.spop(at(1), static_cast<std::size_t>(to_ll(at(2))), t); }));
    }
    if (cmd == "SRANDMEMBER") {
      if (a.size() == 2)
        return ob(off_gil([&] { return c.srandmember(at(1), t); }));
      return ob_list(
          off_gil([&] { return c.srandmember(at(1), to_ll(at(2)), t); }));
    }
    if (cmd == "SMOVE")
      return ob(off_gil([&] { return c.smove(at(1), at(2), at(3), t); }));
    if (cmd == "SINTER") {
      V k(a.begin() + 1, a.end());
      return ob_list(off_gil([&] { return c.sinter(k, t); }));
    }
    if (cmd == "SUNION") {
      V k(a.begin() + 1, a.end());
      return ob_list(off_gil([&] { return c.sunion(k, t); }));
    }
    if (cmd == "SDIFF") {
      V k(a.begin() + 1, a.end());
      return ob_list(off_gil([&] { return c.sdiff(k, t); }));
    }
    if (cmd == "SINTERSTORE") {
      V k(a.begin() + 2, a.end());
      return ob(off_gil([&] { return c.sinterstore(at(1), k, t); }));
    }
    if (cmd == "SUNIONSTORE") {
      V k(a.begin() + 2, a.end());
      return ob(off_gil([&] { return c.sunionstore(at(1), k, t); }));
    }
    if (cmd == "SDIFFSTORE") {
      V k(a.begin() + 2, a.end());
      return ob(off_gil([&] { return c.sdiffstore(at(1), k, t); }));
    }
    if (cmd == "SINTERCARD") {
      // SINTERCARD numkeys key [key ...] [LIMIT limit]
      const auto nkeys = static_cast<std::size_t>(to_ll(at(1)));
      if (a.size() < 2 + nkeys)
        throw ResponseError("ERR wrong number of arguments for 'sintercard'");
      V k(a.begin() + 2, a.begin() + 2 + static_cast<std::ptrdiff_t>(nkeys));
      std::size_t limit = 0;
      if (a.size() > 2 + nkeys) {
        if (a.size() != 4 + nkeys || upper(a[2 + nkeys]) != "LIMIT")
          throw ResponseError("ERR syntax error");
        limit = static_cast<std::size_t>(to_ll(a[3 + nkeys]));
      }
      return ob(off_gil([&] { return c.sintercard(k, limit, t); }));
    }
    if (cmd == "SSCAN") {
      std::uint64_t cursor = static_cast<std::uint64_t>(to_ll(at(2)));
      std::size_t count = 10;
      std::string_view match;
      for (std::size_t i = 3; i < a.size();) {
        const auto tok = upper(a[i]);
        if (tok == "MATCH" && i + 1 < a.size()) {
          match = a[i + 1];
          i += 2;
        } else if (tok == "COUNT" && i + 1 < a.size()) {
          count = static_cast<std::size_t>(to_ll(a[i + 1]));
          i += 2;
        } else {
          throw ResponseError("ERR syntax error");
        }
      }
      auto flat = off_gil(
          [&] { return c.sscan(at(1), cursor, count, match, t); });
      // Redis shape: [cursor, [members...]]
      if (flat.empty()) {
        return nb::make_tuple(py_bytes("0"), nb::list());
      }
      auto members = nb::list();
      for (std::size_t i = 1; i < flat.size(); ++i) {
        members.append(py_bytes(flat[i]));
      }
      return nb::make_tuple(py_bytes(flat[0]), members);
    }

    // ---- sorted sets ---------------------------------------------------------
    if (cmd == "ZADD") {
      std::size_t i = 2;
      while (i < a.size() && is_zadd_flag(a[i])) ++i;  // goblin ZADD ignores NX/XX/GT/LT/CH/INCR
      std::vector<SbeRingClient::Scored> members;
      for (; i + 1 < a.size(); i += 2) members.emplace_back(to_d(a[i]), a[i + 1]);
      return ob(off_gil([&] { return c.zadd(at(1), members, t); }));
    }
    if (cmd == "ZCARD") return ob(off_gil([&] { return c.zcard(at(1), t); }));
    if (cmd == "ZRANGE" || cmd == "ZREVRANGE") {
      const bool rev = cmd == "ZREVRANGE";
      const bool ws = a.size() > 4 && upper(a[4]) == "WITHSCORES";
      const long long lo = to_ll(at(2)), hi = to_ll(at(3));
      if (ws) return ob_flat_scored(off_gil([&] { return c.zrange_withscores(at(1), lo, hi, rev, t); }));
      return ob_list(off_gil([&] { return c.zrange(at(1), lo, hi, rev, t); }));
    }
    if (cmd == "ZRANK") return ob(off_gil([&] { return c.zrank(at(1), at(2), t); }));
    if (cmd == "ZREVRANK") return ob(off_gil([&] { return c.zrevrank(at(1), at(2), t); }));
    if (cmd == "ZREM") { V m(a.begin() + 2, a.end()); return ob(off_gil([&] { return c.zrem(at(1), m, t); })); }
    if (cmd == "ZREMRANGEBYSCORE") {
      const Bound lo = to_bound(at(2)), hi = to_bound(at(3));
      return ob(off_gil([&] { return c.zremrangebyscore(at(1), lo.value, lo.exclusive, hi.value, hi.exclusive, t); }));
    }
    if (cmd == "ZSCORE") return ob(off_gil([&] { return c.zscore(at(1), at(2), t); }));

    // ---- GOBLIN.* natives ----------------------------------------------------
    if (cmd == "GOBLIN.CAD") return ob(off_gil([&] { return c.cad(at(1), at(2), t); }));
    if (cmd == "GOBLIN.CAS")
      return off_gil([&] { return c.cas(at(1), at(2), at(3), t); }) ? py_bytes("OK") : nb::object(nb::int_(0));
    if (cmd == "GOBLIN.CAEXPIRE") return ob(off_gil([&] { return c.caexpire(at(1), at(2), to_ll(at(3)), t); }));
    if (cmd == "GOBLIN.INCREX") return ob(off_gil([&] { return c.increx(at(1), to_ll(at(2)), t); }));
    if (cmd == "GOBLIN.INCRBOUND") return ob(off_gil([&] { return c.incrbound(at(1), to_ll(at(2)), to_ll(at(3)), t); }));
    if (cmd == "GOBLIN.DECRPOS") return ob(off_gil([&] { return c.decrpos(at(1), t); }));
    if (cmd == "GOBLIN.HCAD") return ob(off_gil([&] { return c.hcad(at(1), at(2), at(3), t); }));
    if (cmd == "GOBLIN.HSETGT") return ob(off_gil([&] { return c.hsetgt(at(1), at(2), at(3), t); }));
    if (cmd == "GOBLIN.ZWINDOW")
      return ob(off_gil([&] { return c.zwindow(at(1), to_d(at(2)), to_d(at(3)), to_ll(at(4)), at(5), t); }));
    if (cmd == "GOBLIN.CLAIM") return ob(off_gil([&] { return c.claim(at(1), at(2), at(3), to_ll(at(4)), t); }));
    if (cmd == "GOBLIN.TD_LEADERBOARD_RESCORE") {
      const std::string mode = upper(at(5));
      const std::uint8_t m = mode == "EXP" ? 1 : (mode == "STEP" ? 2 : 0);
      return ob_flat_scored(off_gil([&] {
        return c.td_leaderboard_rescore(at(1), to_d(at(2)), to_d(at(3)), to_ll(at(4)), m, t);
      }));
    }

    // ---- admin ---------------------------------------------------------------
    if (cmd == "GOBLIN.MEMORY") {
      const auto m = off_gil([&] { return c.memory(at(1), t); });
      return m ? ob_flat_pairs(*m) : nb::none();
    }
    if (cmd == "GOBLIN.OPTIMIZE")
      return ob(off_gil([&] { return c.optimize(at(1), a.size() > 2 ? to_d(a[2]) : 0.0, t); }));
    if (cmd == "GOBLIN.SAVE") {
      const bool accel = !(a.size() > 2 && upper(a[2]) == "NOACCEL");
      return py_bytes(off_gil([&] { return c.save(at(1), accel, t); }));
    }
    if (cmd == "GOBLIN.LOAD") return ob(off_gil([&] { return c.load(at(1), t); }));

    throw ResponseError("ERR unknown command '" + cmd + "'");
  }

 protected:
  std::optional<SbeClientT> client_;
};

class Client : public BasicPyClient<SbeRingClient> {
 public:
  Client(const std::string& path, long connect_timeout_ms, std::size_t buffer_size) {
    auto opened =
        SbeRingClient::open(path.c_str(), ms(connect_timeout_ms), buffer_size);
    if (!opened) {
      throw RingError("cannot open ring '" + path +
                      "' (is goblin-core running with --ring " + path + " ...?)");
    }
    client_.emplace(std::move(*opened));
  }
};

#if defined(GOBLIN_HAS_RDMA)
class RdmaClient : public BasicPyClient<SbeRdmaClient> {
 public:
  RdmaClient(const std::string& host, int port, std::uint64_t ring_bytes,
             long connect_timeout_ms, std::size_t buffer_size) {
    if (port <= 0 || port > 65535) {
      throw RingError("RdmaClient: port out of range");
    }
    if (ring_bytes == 0) {
      throw RingError("RdmaClient: ring_bytes must be positive");
    }
    std::string error;
    auto opened = SbeRdmaClient::open(
        std::string_view(host), static_cast<std::uint16_t>(port), ring_bytes,
        ms(connect_timeout_ms), buffer_size, &error);
    if (!opened) {
      throw RingError("cannot open RDMA to " + host + ":" + std::to_string(port) +
                      (error.empty() ? "" : (" (" + error + ")")));
    }
    client_.emplace(std::move(*opened));
  }
};
#endif

#if defined(GOBLIN_HAS_EXASOCK)
class ExasockClient : public BasicPyClient<SbeExasockClient> {
 public:
  ExasockClient(const std::string& host, int port, long connect_timeout_ms,
                std::size_t buffer_size, bool require_loaded, int ate_id) {
    if (port <= 0 || port > 65535) {
      throw RingError("ExasockClient: port out of range");
    }
    goblin::core::exasock::ConnectOptions options;
    options.require_loaded = require_loaded;
    options.ate_id = ate_id;
    std::string error;
    auto opened = SbeExasockClient::open(
        std::string_view(host), static_cast<std::uint16_t>(port),
        ms(connect_timeout_ms), buffer_size, options, &error);
    if (!opened) {
      throw RingError("cannot open ExaSock TCP to " + host + ":" +
                      std::to_string(port) +
                      (error.empty() ? "" : (" (" + error + ")")));
    }
    client_.emplace(std::move(*opened));
  }
};
#endif

}  // namespace

NB_MODULE(_goblin_core, m) {
  m.doc() =
      "SBE client for goblin-core over shared-memory ring, RDMA, or ExaSock TCP.";

  nb::exception<ResponseError>(m, "ResponseError");
  nb::exception<RingError>(m, "RingError");

  nb::class_<Client>(m, "Client")
      .def(nb::init<const std::string&, long, std::size_t>(), nb::arg("path"),
           nb::arg("connect_timeout_ms") = 2000, nb::arg("buffer_size") = 16 * 1024)
      .def("buffer_size", &Client::buffer_size,
           "Request/reply buffer size in bytes (grown to the ring capacity on open).")
      .def("execute_command", &Client::execute_command, nb::arg("args"),
           nb::arg("timeout_ms") = 5000,
           "Send one already-encoded command (list[bytes]) over SBE and return the reply.");

#if defined(GOBLIN_HAS_RDMA)
  m.attr("HAS_RDMA") = true;
  nb::class_<RdmaClient>(m, "RdmaClient")
      .def(nb::init<const std::string&, int, std::uint64_t, long, std::size_t>(),
           nb::arg("host"), nb::arg("port"), nb::arg("ring_bytes"),
           nb::arg("connect_timeout_ms") = 5000,
           nb::arg("buffer_size") = 64 * 1024)
      .def("buffer_size", &RdmaClient::buffer_size)
      .def("execute_command", &RdmaClient::execute_command, nb::arg("args"),
           nb::arg("timeout_ms") = 5000,
           "Send one already-encoded command over SBE/RDMA.");
#else
  m.attr("HAS_RDMA") = false;
#endif

#if defined(GOBLIN_HAS_EXASOCK)
  m.attr("HAS_EXASOCK") = true;
  nb::class_<ExasockClient>(m, "ExasockClient")
      .def(nb::init<const std::string&, int, long, std::size_t, bool, int>(),
           nb::arg("host"), nb::arg("port"),
           nb::arg("connect_timeout_ms") = 2000,
           nb::arg("buffer_size") = 64 * 1024,
           nb::arg("require_loaded") = false, nb::arg("ate_id") = -1)
      .def("buffer_size", &ExasockClient::buffer_size)
      .def("execute_command", &ExasockClient::execute_command, nb::arg("args"),
           nb::arg("timeout_ms") = 5000,
           "Send one already-encoded command over SBE/TCP (ExaSock-accelerated when "
           "run under the exasock wrapper).");
#else
  m.attr("HAS_EXASOCK") = false;
#endif
}
