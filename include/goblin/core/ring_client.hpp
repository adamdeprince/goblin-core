#pragma once

// A header-only client for the shared-memory ring buffers (see ring_buffer.hpp).
//
// It is the reference for how to talk to a ring: encode a RESP command, push it
// onto the SQ (submission queue), then busy-poll the CQ (completion queue) for the
// reply. Being header-only, it doubles as a tiny test/benchmark harness -- just
// `#include` it, `RingClient::open("/tmp/a")`, and call `command({"SET","k","v"})`.
//
// A ring is single-writer/single-reader, so at most one RingClient may drive a
// given ring file at a time (the server is the single reader on the other side).

#include "goblin/core/ring_buffer.hpp"

#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core::ring {

// Encode a command as a RESP array of bulk strings (the wire form redis clients
// send): *<n>\r\n $<len>\r\n<arg>\r\n ...
[[nodiscard]] inline std::string encode_command(
    std::span<const std::string_view> args) {
  std::string out;
  out.reserve(16 * args.size());
  out.push_back('*');
  out.append(std::to_string(args.size()));
  out.append("\r\n");
  for (const std::string_view arg : args) {
    out.push_back('$');
    out.append(std::to_string(arg.size()));
    out.append("\r\n");
    out.append(arg);
    out.append("\r\n");
  }
  return out;
}

// The byte length of the first complete RESP reply in `s` starting at `pos`, or
// nullopt if `s` does not yet hold a whole reply. Recurses through arrays. Handles
// RESP2 and RESP3 reply types emitted by the server. Aggregate types recurse;
// attributes include the reply they decorate.
[[nodiscard]] inline std::optional<std::size_t> reply_end(std::string_view s,
                                                          std::size_t pos = 0) {
  if (pos >= s.size()) {
    return std::nullopt;
  }
  const std::size_t eol = s.find("\r\n", pos);
  if (eol == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t line_end = eol + 2;
  const auto parse_count = [&](long long& out) -> bool {
    const std::string_view digits = s.substr(pos + 1, eol - (pos + 1));
    const auto [ptr, ec] =
        std::from_chars(digits.data(), digits.data() + digits.size(), out);
    return ec == std::errc{} && ptr == digits.data() + digits.size();
  };
  switch (s[pos]) {
    case '+':
    case '-':
    case ':':
    case ',':  // RESP3 double
    case '(':  // RESP3 big number
    case '#':  // RESP3 boolean
    case '_':  // RESP3 null
      return line_end;
    case '$':
    case '!':  // RESP3 blob error
    case '=': {  // RESP3 verbatim string
      long long len = 0;
      if (!parse_count(len)) {
        return std::nullopt;
      }
      if (len < 0) {
        return line_end;  // null bulk string
      }
      const std::size_t need = line_end + static_cast<std::size_t>(len) + 2;
      return need <= s.size() ? std::optional<std::size_t>(need) : std::nullopt;
    }
    case '*':
    case '~':  // RESP3 set
    case '>': {  // RESP3 push
      long long n = 0;
      if (!parse_count(n)) {
        return std::nullopt;
      }
      if (n < 0) {
        return line_end;  // null array
      }
      std::size_t cur = line_end;
      for (long long i = 0; i < n; ++i) {
        const auto sub = reply_end(s, cur);
        if (!sub) {
          return std::nullopt;
        }
        cur = *sub;
      }
      return cur;
    }
    case '%':
    case '|': {  // RESP3 map / attributes
      long long n = 0;
      if (!parse_count(n) || n < 0) {
        return std::nullopt;
      }
      std::size_t cur = line_end;
      for (long long i = 0; i < n * 2; ++i) {
        const auto sub = reply_end(s, cur);
        if (!sub) {
          return std::nullopt;
        }
        cur = *sub;
      }
      if (s[pos] == '|') {
        return reply_end(s, cur);
      }
      return cur;
    }
    default:
      return line_end;  // unknown type: treat the line as the whole reply
  }
}

class RingClient {
 public:
  // Open an existing ring file. Retries briefly so a client may be started right
  // after the server (the file appears and is initialized asynchronously).
  [[nodiscard]] static std::optional<RingClient> open(
      const char* path,
      std::chrono::milliseconds wait = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
      if (auto m = Mapping::open(path)) {
        // Reconnect handshake (symmetric with SbeRingClient::open): claim a fresh epoch
        // and wait for the server to drain the ring -- discarding whatever a dead
        // predecessor left (unread replies, a half-parsed command) -- and ack, so we
        // start clean regardless of how the previous client on this ring exited.
        const std::uint64_t epoch = m->request_reconnect();
        while (!m->reconnect_acked(epoch)) {
          if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
          }
          cpu_relax();
        }
        return RingClient(std::move(*m));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      cpu_relax();
    }
  }

  // Push raw request bytes onto the SQ (they need not be a whole command; the
  // server reassembles the byte stream). Spins for space if the ring is full.
  void send_raw(std::string_view bytes,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    sq_.send(bytes, [&] { return std::chrono::steady_clock::now() >= deadline; });
  }

  void send(std::span<const std::string_view> args) {
    send_raw(encode_command(args));
  }

  // Busy-poll the CQ until a full RESP reply is available, then return it. nullopt
  // on timeout. Extra bytes (e.g. a pipelined next reply) are retained for the
  // following call.
  [[nodiscard]] std::optional<std::string> read_reply(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Amortize the deadline clock read across spins (same rationale as
    // SbeRingClient::read_frame): the empty-CQ wait is the p99 path, and a
    // steady_clock sample every pause is a large fraction of a sub-µs RTT.
    unsigned spins = 0;
    for (;;) {
      if (const auto end = reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      if (auto rec = cq_.peek()) {
        pending_.append(*rec);
        cq_.pop();
        continue;  // re-check without pausing -- more may be ready
      }
      if ((++spins & 63u) == 0 && std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      // Adaptive spin-then-park on the CQ tail (macOS); pure relax elsewhere.
      cq_.wait_for_record();
    }
  }

  // Send one command and return its reply (the common request/response call).
  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> args) {
    send(args);
    return read_reply();
  }
  [[nodiscard]] std::optional<std::string> command(
      std::initializer_list<std::string_view> args) {
    return command(std::span<const std::string_view>(args.begin(), args.size()));
  }

  // Move any ready CQ records into the pending buffer without blocking. A streaming
  // writer must service the CQ so the server never stalls on a full CQ -- with a full
  // SQ that would deadlock a pure writer. Returns the number of bytes drained.
  std::size_t drain_cq() {
    std::size_t drained = 0;
    while (auto rec = cq_.peek()) {
      pending_.append(*rec);
      drained += rec->size();
      cq_.pop();
    }
    return drained;
  }

  // Non-blocking read: return the next complete reply if one is already available
  // (draining ready CQ records first), else nullopt. Lets a streaming writer count
  // and discard replies as they arrive, so buffered replies never grow unbounded.
  [[nodiscard]] std::optional<std::string> try_read_reply() {
    for (;;) {
      if (const auto end = reply_end(pending_)) {
        std::string reply = pending_.substr(0, *end);
        pending_.erase(0, *end);
        return reply;
      }
      auto rec = cq_.peek();
      if (!rec) {
        return std::nullopt;
      }
      pending_.append(*rec);
      cq_.pop();
    }
  }

  // Encode a command and push it as ONE atomic ring record (Producer::send_record):
  // block until the ring has room -- draining the CQ while blocked so a stalled
  // server (one waiting on a full CQ) can advance -- and throw std::length_error if
  // the encoded command is larger than the ring can ever hold. This is the
  // streaming/pipe primitive; cf. command(), which is a synchronous request/reply.
  // Throws std::runtime_error on timeout.
  void send_command_atomic(
      std::span<const std::string_view> args,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    const std::string bytes = encode_command(args);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool pushed = sq_.send_record(bytes, [&] {
      drain_cq();  // service replies so a blocked server can make progress
      return std::chrono::steady_clock::now() >= deadline;
    });
    if (!pushed) {
      throw std::runtime_error(
          "RingClient: timed out streaming a command (server stalled or gone)");
    }
  }

  [[nodiscard]] const Mapping& mapping() const noexcept { return mapping_; }

 private:
  explicit RingClient(Mapping&& m)
      : mapping_(std::move(m)),
        sq_(mapping_.sq_producer()),
        cq_(mapping_.cq_consumer()) {}

  Mapping mapping_;
  Producer sq_;
  Consumer cq_;
  std::string pending_;  // CQ bytes not yet consumed as a whole reply
};

}  // namespace goblin::core::ring
