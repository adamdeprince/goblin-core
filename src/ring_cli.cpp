// redis-cli-ring: RESP client for goblin-core over ring, RDMA, or ExaSock TCP.
//
//   redis-cli-ring /tmp/a SET foo bar                 # shared-memory ring
//   redis-cli-ring --ring /tmp/a PING
//   redis-cli-ring --rdma 10.88.88.1 6380 1mb PING    # needs GOBLIN_HAS_RDMA
//   redis-cli-ring --exasock 10.99.99.1 6379 PING     # needs GOBLIN_HAS_EXASOCK
//   redis-cli-ring /tmp/a                             # interactive
//   redis-cli-ring /tmp/a -f cmds.txt                 # stream file (like --pipe)
//
// Transport is selected by flags (or a bare ring path). RDMA and ExaSock are
// compile-time optional; without those flags the binary still builds for rings.

#include "goblin/core/ring_client.hpp"
#if defined(GOBLIN_HAS_RDMA)
#include "goblin/core/rdma_client.hpp"
#endif
#if defined(GOBLIN_HAS_EXASOCK)
#include "goblin/core/exasock_client.hpp"
#endif

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using goblin::core::ring::RingClient;

// Pretty-print one complete RESP2/RESP3 reply, redis-cli style.
std::size_t print_reply(std::string_view s, std::size_t pos,
                        const std::string& indent) {
  const std::size_t eol = s.find("\r\n", pos);
  if (eol == std::string_view::npos) {
    std::cout << "(malformed reply)\n";
    return s.size();
  }
  const std::string_view line = s.substr(pos + 1, eol - (pos + 1));
  const std::size_t after = eol + 2;
  const auto to_ll = [](std::string_view v) {
    long long n = 0;
    std::from_chars(v.data(), v.data() + v.size(), n);
    return n;
  };
  switch (s[pos]) {
    case '+':
      std::cout << line << '\n';
      return after;
    case '-':
      std::cout << "(error) " << line << '\n';
      return after;
    case ':':
      std::cout << "(integer) " << line << '\n';
      return after;
    case ',':
      std::cout << "(double) " << line << '\n';
      return after;
    case '(':
      std::cout << "(big number) " << line << '\n';
      return after;
    case '#':
      std::cout << (line == "t" ? "(true)" : "(false)") << '\n';
      return after;
    case '_':
      std::cout << "(nil)\n";
      return after;
    case '$':
    case '!':
    case '=': {
      const long long len = to_ll(line);
      if (len < 0) {
        std::cout << "(nil)\n";
        return after;
      }
      if (s[pos] == '!') {
        std::cout << "(error) ";
      } else if (s[pos] == '=') {
        std::cout << "(verbatim) ";
      }
      std::cout << '"' << s.substr(after, static_cast<std::size_t>(len))
                << "\"\n";
      return after + static_cast<std::size_t>(len) + 2;
    }
    case '*':
    case '~':
    case '>': {
      const long long n = to_ll(line);
      if (n < 0) {
        std::cout << "(nil)\n";
        return after;
      }
      if (n == 0) {
        std::cout << "(empty array)\n";
        return after;
      }
      std::size_t cur = after;
      for (long long i = 0; i < n; ++i) {
        std::cout << indent << (i + 1) << ") ";
        cur = print_reply(s, cur, indent + "   ");
      }
      return cur;
    }
    case '%':
    case '|': {
      const long long n = to_ll(line);
      std::cout << (s[pos] == '%' ? "(map)\n" : "(attributes)\n");
      std::size_t cur = after;
      for (long long i = 0; i < n; ++i) {
        std::cout << indent << (i + 1) << ") key: ";
        cur = print_reply(s, cur, indent + "   ");
        std::cout << indent << "   value: ";
        cur = print_reply(s, cur, indent + "   ");
      }
      return s[pos] == '|' ? print_reply(s, cur, indent) : cur;
    }
    default:
      std::cout << line << '\n';
      return after;
  }
}

bool split_args(std::string_view line, std::vector<std::string>& out) {
  out.clear();
  const std::size_t n = line.size();
  std::size_t i = 0;
  while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i < n && line[i] == '#') return true;
  const auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= n) break;
    std::string cur;
    bool in_dq = false, in_sq = false, token_done = false;
    bool have_token = false;
    while (i < n && !token_done) {
      const char c = line[i];
      if (in_dq) {
        if (c == '\\' && i + 1 < n) {
          const char e = line[i + 1];
          if (e == 'x' && i + 3 < n && hexval(line[i + 2]) >= 0 &&
              hexval(line[i + 3]) >= 0) {
            cur.push_back(static_cast<char>((hexval(line[i + 2]) << 4) |
                                            hexval(line[i + 3])));
            i += 4;
          } else {
            switch (e) {
              case 'n':
                cur.push_back('\n');
                break;
              case 'r':
                cur.push_back('\r');
                break;
              case 't':
                cur.push_back('\t');
                break;
              case 'b':
                cur.push_back('\b');
                break;
              case 'a':
                cur.push_back('\a');
                break;
              default:
                cur.push_back(e);
                break;
            }
            i += 2;
          }
        } else if (c == '"') {
          in_dq = false;
          ++i;
        } else {
          cur.push_back(c);
          ++i;
        }
      } else if (in_sq) {
        if (c == '\\' && i + 1 < n && line[i + 1] == '\'') {
          cur.push_back('\'');
          i += 2;
        } else if (c == '\'') {
          in_sq = false;
          ++i;
        } else {
          cur.push_back(c);
          ++i;
        }
      } else if (std::isspace(static_cast<unsigned char>(c))) {
        token_done = true;
      } else if (c == '"') {
        in_dq = true;
        have_token = true;
        ++i;
      } else if (c == '\'') {
        in_sq = true;
        have_token = true;
        ++i;
      } else {
        cur.push_back(c);
        have_token = true;
        ++i;
      }
    }
    if (in_dq || in_sq) return false;
    if (have_token || !cur.empty()) out.push_back(std::move(cur));
  }
  return true;
}

// Type-erased RESP client over ring / RDMA / ExaSock.
class AnyClient {
 public:
  explicit AnyClient(RingClient client) : backend_(std::move(client)) {}
#if defined(GOBLIN_HAS_RDMA)
  explicit AnyClient(goblin::core::rdma::RdmaClient client)
      : backend_(std::move(client)) {}
#endif
#if defined(GOBLIN_HAS_EXASOCK)
  explicit AnyClient(goblin::core::exasock::ExasockClient client)
      : backend_(std::move(client)) {}
#endif

  [[nodiscard]] std::optional<std::string> command(
      std::span<const std::string_view> args) {
    return std::visit(
        [&](auto& c) -> std::optional<std::string> { return c.command(args); },
        backend_);
  }

  void send_command(std::span<const std::string_view> args) {
    std::visit(
        [&](auto& c) {
          using T = std::decay_t<decltype(c)>;
          if constexpr (std::is_same_v<T, RingClient>) {
            c.send_command_atomic(args);
          } else {
            c.send_pipelined(args);
          }
        },
        backend_);
  }

  [[nodiscard]] std::optional<std::string> try_read_reply() {
    return std::visit(
        [](auto& c) -> std::optional<std::string> {
          return c.try_read_reply();
        },
        backend_);
  }

  [[nodiscard]] std::optional<std::string> read_reply() {
    return std::visit(
        [](auto& c) -> std::optional<std::string> { return c.read_reply(); },
        backend_);
  }

 private:
  using Backend = std::variant<
      RingClient
#if defined(GOBLIN_HAS_RDMA)
      ,
      goblin::core::rdma::RdmaClient
#endif
#if defined(GOBLIN_HAS_EXASOCK)
      ,
      goblin::core::exasock::ExasockClient
#endif
      >;
  Backend backend_;
};

int run_command(AnyClient& client, const std::vector<std::string_view>& args) {
  try {
    const auto reply = client.command(args);
    if (!reply) {
      std::cerr << "redis-cli-ring: timed out waiting for a reply\n";
      return 1;
    }
    print_reply(*reply, 0, "");
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "redis-cli-ring: " << ex.what() << '\n';
    return 1;
  }
}

std::string reply_summary(const std::string& reply) {
  const std::size_t eol = reply.find("\r\n");
  const std::size_t end = eol == std::string::npos ? reply.size() : eol;
  return reply.substr(1, end - 1);
}

int stream_commands(AnyClient& client, std::istream& in,
                    const std::string& source) {
  std::size_t line_no = 0, sent = 0, replies = 0, errors = 0, skipped = 0,
              shown = 0;
  constexpr std::size_t kMaxShown = 20;
  bool aborted = false;

  const auto note_reply = [&](const std::string& r) {
    ++replies;
    if (!r.empty() && r[0] == '-') {
      ++errors;
      if (shown < kMaxShown) {
        std::cerr << "  (error) " << reply_summary(r) << '\n';
        ++shown;
      }
    }
  };

  std::string line;
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> tokens;
    if (!split_args(line, tokens)) {
      std::cerr << source << ':' << line_no
                << ": unbalanced quotes -- skipped\n";
      ++skipped;
      continue;
    }
    if (tokens.empty()) continue;
    const std::vector<std::string_view> args(tokens.begin(), tokens.end());
    try {
      client.send_command(args);
    } catch (const std::length_error& e) {
      std::cerr << source << ':' << line_no
                << ": command too large for transport: " << e.what() << '\n';
      aborted = true;
      break;
    } catch (const std::exception& e) {
      std::cerr << source << ':' << line_no << ": " << e.what() << '\n';
      aborted = true;
      break;
    }
    ++sent;
    while (auto r = client.try_read_reply()) note_reply(*r);
  }

  while (replies < sent) {
    auto r = client.read_reply();
    if (!r) {
      std::cerr << "  timed out with " << (sent - replies)
                << " repl(y/ies) outstanding\n";
      break;
    }
    note_reply(*r);
  }

  if (errors > kMaxShown)
    std::cerr << "  ... and " << (errors - shown) << " more error(s)\n";
  std::cout << "streamed " << sent << " command(s) from " << source << "; "
            << replies << " reply(ies), " << errors << " error(s)";
  if (skipped) std::cout << ", " << skipped << " line(s) skipped";
  if (aborted) std::cout << " (aborted)";
  std::cout << '\n';
  return (aborted || errors > 0) ? 1 : 0;
}

void print_usage() {
  std::cerr
      << "usage:\n"
      << "  redis-cli-ring <ring-path> [COMMAND ARG ...]\n"
      << "  redis-cli-ring --ring <path> [COMMAND ARG ...]\n"
#if defined(GOBLIN_HAS_RDMA)
      << "  redis-cli-ring --rdma <host> <port> <size> [COMMAND ARG ...]\n"
#endif
#if defined(GOBLIN_HAS_EXASOCK)
      << "  redis-cli-ring --exasock <host> <port> [COMMAND ARG ...]\n"
#endif
      << "  redis-cli-ring <transport...> -f <file>|-\n"
      << "\n"
      << "Transports (server must expose a matching target):\n"
      << "  --ring PATH           shared-memory ring (default if PATH is bare)\n"
#if defined(GOBLIN_HAS_RDMA)
      << "  --rdma HOST PORT SIZE polled RDMA rings (size e.g. 64kb, 1mb)\n"
#else
      << "  --rdma ...            (unavailable: build with -DGOBLIN_CORE_ENABLE_RDMA=ON)\n"
#endif
#if defined(GOBLIN_HAS_EXASOCK)
      << "  --exasock HOST PORT   priority TCP / ExaSock (run under `exasock` for bypass)\n"
#else
      << "  --exasock ...         (unavailable: build with -DGOBLIN_CORE_ENABLE_EXASOCK=ON)\n"
#endif
      << "\n"
      << "No COMMAND enters interactive mode. -f streams one command per line.\n";
}

[[nodiscard]] bool parse_u16(std::string_view text, std::uint16_t& out) {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
      value > 65535) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

struct Opened {
  std::optional<AnyClient> client;
  std::string label;
  int argc_consumed{0};  // argv indices consumed for transport (incl. flags)
};

// Parse transport from argv starting at index 1. Returns how many argv slots
// after argv[0] belong to the transport (so command args start at 1+consumed).
[[nodiscard]] Opened open_from_argv(int argc, char** argv) {
  Opened opened;
  if (argc < 2) {
    return opened;
  }

  const std::string_view a1 = argv[1];

  if (a1 == "--ring") {
    if (argc < 3) {
      std::cerr << "redis-cli-ring: --ring requires PATH\n";
      return opened;
    }
    auto client = RingClient::open(argv[2]);
    if (!client) {
      std::cerr << "redis-cli-ring: cannot open ring " << argv[2]
                << " (is the server running with --ring " << argv[2]
                << " ...?)\n";
      return opened;
    }
    opened.client.emplace(std::move(*client));
    opened.label = std::string("ring ") + argv[2];
    opened.argc_consumed = 2;
    return opened;
  }

  if (a1 == "--rdma") {
#if defined(GOBLIN_HAS_RDMA)
    if (argc < 5) {
      std::cerr << "redis-cli-ring: --rdma requires HOST PORT SIZE\n";
      return opened;
    }
    std::uint16_t port = 0;
    const auto size = goblin::core::ring::parse_size(argv[4]);
    if (!parse_u16(argv[3], port) || !size || *size == 0) {
      std::cerr << "redis-cli-ring: invalid --rdma port or size\n";
      return opened;
    }
    std::string error;
    auto client = goblin::core::rdma::RdmaClient::open(
        argv[2], port, *size, std::chrono::seconds(5), &error);
    if (!client) {
      std::cerr << "redis-cli-ring: RDMA open failed: " << error << '\n';
      return opened;
    }
    opened.client.emplace(std::move(*client));
    opened.label = std::string("rdma ") + argv[2] + ':' + argv[3];
    opened.argc_consumed = 4;
    return opened;
#else
    std::cerr << "redis-cli-ring: --rdma unavailable in this build "
                 "(-DGOBLIN_CORE_ENABLE_RDMA=ON + libibverbs/librdmacm)\n";
    return opened;
#endif
  }

  if (a1 == "--exasock") {
#if defined(GOBLIN_HAS_EXASOCK)
    if (argc < 4) {
      std::cerr << "redis-cli-ring: --exasock requires HOST PORT\n";
      return opened;
    }
    std::uint16_t port = 0;
    if (!parse_u16(argv[3], port)) {
      std::cerr << "redis-cli-ring: invalid --exasock port\n";
      return opened;
    }
    std::string error;
    goblin::core::exasock::ConnectOptions options;
    auto client = goblin::core::exasock::ExasockClient::open(
        argv[2], port, std::chrono::seconds(5), options, &error);
    if (!client) {
      std::cerr << "redis-cli-ring: ExaSock open failed: " << error << '\n';
      return opened;
    }
    if (goblin::core::exasock::loaded()) {
      std::cerr << "redis-cli-ring: ExaSock "
                << goblin::core::exasock::version_text()
                << (client->accelerated() ? " (accelerated)\n"
                                          : " (not accelerated)\n");
    }
    opened.client.emplace(std::move(*client));
    opened.label = std::string("exasock ") + argv[2] + ':' + argv[3];
    opened.argc_consumed = 3;
    return opened;
#else
    std::cerr << "redis-cli-ring: --exasock unavailable in this build "
                 "(-DGOBLIN_CORE_ENABLE_EXASOCK=ON + ExaSock SDK)\n";
    return opened;
#endif
  }

  // Bare path: shared-memory ring (historical redis-cli-ring usage).
  if (a1.starts_with("-")) {
    std::cerr << "redis-cli-ring: unknown option " << a1 << '\n';
    return opened;
  }
  auto client = RingClient::open(argv[1]);
  if (!client) {
    std::cerr << "redis-cli-ring: cannot open ring " << argv[1]
              << " (is the server running with --ring " << argv[1] << " ...?)\n";
    return opened;
  }
  opened.client.emplace(std::move(*client));
  opened.label = std::string("ring ") + argv[1];
  opened.argc_consumed = 1;
  return opened;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" ||
      std::string_view(argv[1]) == "-h") {
    print_usage();
    return argc < 2 ? 2 : 0;
  }

  auto opened = open_from_argv(argc, argv);
  if (!opened.client) {
    return 1;
  }
  AnyClient& client = *opened.client;
  const int cmd0 = 1 + opened.argc_consumed;

  // Stream mode: ... -f <file>|-
  if (cmd0 + 1 < argc &&
      (std::strcmp(argv[cmd0], "-f") == 0 ||
       std::strcmp(argv[cmd0], "--file") == 0)) {
    const char* file = argv[cmd0 + 1];
    if (std::strcmp(file, "-") == 0) {
      return stream_commands(client, std::cin, "<stdin>");
    }
    std::ifstream in(file);
    if (!in) {
      std::cerr << "redis-cli-ring: cannot open command file " << file << '\n';
      return 1;
    }
    return stream_commands(client, in, file);
  }

  if (cmd0 < argc) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc - cmd0));
    for (int i = cmd0; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    return run_command(client, args);
  }

  std::string line;
  const bool tty = ::isatty(fileno(stdin)) != 0;
  if (tty) {
    std::cout << "redis-cli-ring " << opened.label
              << " (one command per line, Ctrl-D to quit)\n";
  }
  while (true) {
    if (tty) {
      std::cout << "> " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      break;
    }
    std::vector<std::string> tokens;
    if (!split_args(line, tokens)) {
      std::cerr << "(unbalanced quotes)\n";
      continue;
    }
    if (tokens.empty()) {
      continue;
    }
    std::vector<std::string_view> args(tokens.begin(), tokens.end());
    (void)run_command(client, args);
  }
  return 0;
}
