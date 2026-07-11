// nanobind extension backing the goblin_core Python package: a thin, fast client
// that speaks to a goblin-core server over a shared-memory ring buffer.
//
// The systems work lives here in C++ -- encode the RESP request, hand it to the
// ring, busy-poll the completion queue with the PAUSE/YIELD relax hint (inside
// RingClient, so the spinning core lets sibling cores land their stores), and
// parse the RESP reply into native Python objects. The redis-py-compatible method
// surface (get/set/zadd/...) is a thin Python layer on top (goblin_core/__init__.py).
//
// The GIL is released for the whole ring round trip, so a busy-poll on one thread
// never starves other Python threads.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include "goblin/core/ring_client.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nb = nanobind;
using goblin::core::ring::RingClient;

namespace {

// A RESP error reply ("-...") -- mapped to the Python ResponseError, the same
// shape redis-py raises. A transport failure (ring cannot open, reply timed out)
// maps to RingError.
struct ResponseError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct RingError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[nodiscard]] long long parse_int(std::string_view s) {
  long long v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

// Parse one complete RESP2 reply beginning at `pos` into a Python object,
// advancing `pos`. `s` is assumed to hold a whole reply (RingClient frames it).
//   +simple  -> bytes (redis-py returns b'OK'; the Python layer post-processes)
//   -error   -> raise ResponseError
//   :int     -> int
//   $bulk    -> bytes, or None for a null bulk
//   *array   -> list, or None for a null array
nb::object parse_reply(std::string_view s, std::size_t& pos) {
  const std::size_t eol = s.find("\r\n", pos);
  const char type = s[pos];
  const std::string_view line = s.substr(pos + 1, eol - (pos + 1));
  pos = eol + 2;
  switch (type) {
    case '+':
      return nb::bytes(line.data(), line.size());
    case '-':
      throw ResponseError(std::string(line));
    case ':':
      return nb::int_(parse_int(line));
    case '$': {
      const long long len = parse_int(line);
      if (len < 0) {
        return nb::none();
      }
      nb::object out = nb::bytes(s.data() + pos, static_cast<std::size_t>(len));
      pos += static_cast<std::size_t>(len) + 2;
      return out;
    }
    case '*': {
      const long long n = parse_int(line);
      if (n < 0) {
        return nb::none();
      }
      nb::list out;
      for (long long i = 0; i < n; ++i) {
        out.append(parse_reply(s, pos));
      }
      return out;
    }
    default:
      return nb::bytes(line.data(), line.size());
  }
}

class Client {
 public:
  Client(const std::string& path, long connect_timeout_ms) {
    auto opened =
        RingClient::open(path.c_str(), std::chrono::milliseconds(connect_timeout_ms));
    if (!opened) {
      throw RingError("cannot open ring '" + path +
                      "' (is goblin-core running with --ring " + path + " ...?)");
    }
    client_.emplace(std::move(*opened));
  }

  // args: a list of bytes (one per command word), already encoded by the Python
  // layer. Returns the parsed reply.
  nb::object execute_command(nb::list args, long timeout_ms) {
    // Build the RESP request while the GIL is held (we read Python bytes here).
    std::string request;
    const Py_ssize_t argc = PyList_GET_SIZE(args.ptr());
    request.push_back('*');
    request.append(std::to_string(argc));
    request.append("\r\n");
    for (Py_ssize_t i = 0; i < argc; ++i) {
      PyObject* item = PyList_GET_ITEM(args.ptr(), i);
      if (!PyBytes_Check(item)) {
        throw std::invalid_argument("command arguments must be bytes");
      }
      const char* data = PyBytes_AS_STRING(item);
      const Py_ssize_t n = PyBytes_GET_SIZE(item);
      request.push_back('$');
      request.append(std::to_string(n));
      request.append("\r\n");
      request.append(data, static_cast<std::size_t>(n));
      request.append("\r\n");
    }

    // Release the GIL for the whole ring round trip: the busy-poll (with its
    // PAUSE/YIELD relax) runs off the GIL, so other Python threads keep running.
    std::string reply;
    bool ok = false;
    {
      nb::gil_scoped_release release;
      client_->send_raw(request, std::chrono::milliseconds(timeout_ms));
      if (auto r = client_->read_reply(std::chrono::milliseconds(timeout_ms))) {
        reply = std::move(*r);
        ok = true;
      }
    }
    if (!ok) {
      throw RingError("timed out waiting for a ring reply");
    }
    std::size_t pos = 0;
    return parse_reply(reply, pos);
  }

 private:
  std::optional<RingClient> client_;
};

}  // namespace

NB_MODULE(_goblin_core, m) {
  m.doc() = "Shared-memory ring-buffer client for goblin-core (C++ core).";

  nb::exception<ResponseError>(m, "ResponseError");
  nb::exception<RingError>(m, "RingError");

  nb::class_<Client>(m, "Client")
      .def(nb::init<const std::string&, long>(), nb::arg("path"),
           nb::arg("connect_timeout_ms") = 2000)
      .def("execute_command", &Client::execute_command, nb::arg("args"),
           nb::arg("timeout_ms") = 5000,
           "Send one already-encoded command (list[bytes]) and return the reply.");
}
