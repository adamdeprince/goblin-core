// redis-cli-ring: a client that talks to goblin-core over a shared-memory ring
// buffer instead of a socket. It shows the whole client side of the protocol --
// open the ring, encode a RESP command, push it on the SQ, then busy-poll the CQ
// for the reply -- all on top of the header-only client in
// goblin/core/ring_client.hpp.
//
//   redis-cli-ring /tmp/a SET foo bar      # one-shot: run one command, print reply
//   redis-cli-ring /tmp/a                  # interactive: a command per stdin line
//   redis-cli-ring /tmp/a -f cmds.txt      # stream a file of commands into the ring
//   redis-cli-ring /tmp/a -f -             # stream commands from stdin
//
// The file/stdin stream mode ("-f", like redis-cli --pipe) reads one command per
// line and pushes each as a single atomic ring record. It relies on the ring
// library's backpressure: Producer::send_record blocks when the ring is full and
// resumes the instant there is room for the next command, and throws
// std::length_error if a command is larger than the ring can ever hold (so a
// mis-sized ring fails loudly instead of spinning forever). Replies are drained as
// the stream runs, so a full CQ never stalls the server.
//
// The server must already be running with a matching `--ring /tmp/a <size>`.

#include "goblin/core/ring_client.hpp"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using goblin::core::ring::RingClient;

// Pretty-print one complete RESP2/RESP3 reply, redis-cli style. Returns the number
// of bytes consumed so aggregate replies can print their elements in sequence.
std::size_t print_reply(std::string_view s, std::size_t pos, const std::string& indent) {
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
      std::cout << '"' << s.substr(after, static_cast<std::size_t>(len)) << "\"\n";
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

// Split one line into arguments, redis-cli style: whitespace-separated, with
// "double" quotes (escapes \n \r \t \b \a \xHH \\ \") and 'single' quotes (literal,
// only \' recognized). Returns false on an unterminated quote. A blank/comment line
// (starting with '#') yields no args. Empty quoted strings ("") are valid arguments.
bool split_args(std::string_view line, std::vector<std::string>& out) {
  out.clear();
  const std::size_t n = line.size();
  std::size_t i = 0;
  while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
  if (i < n && line[i] == '#') return true;  // comment line
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
    bool have_token = false;  // saw at least an opening quote or a char
    while (i < n && !token_done) {
      const char c = line[i];
      if (in_dq) {
        if (c == '\\' && i + 1 < n) {
          const char e = line[i + 1];
          if (e == 'x' && i + 3 < n && hexval(line[i + 2]) >= 0 && hexval(line[i + 3]) >= 0) {
            cur.push_back(static_cast<char>((hexval(line[i + 2]) << 4) | hexval(line[i + 3])));
            i += 4;
          } else {
            switch (e) {
              case 'n': cur.push_back('\n'); break;
              case 'r': cur.push_back('\r'); break;
              case 't': cur.push_back('\t'); break;
              case 'b': cur.push_back('\b'); break;
              case 'a': cur.push_back('\a'); break;
              default: cur.push_back(e); break;
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
    if (in_dq || in_sq) return false;  // unterminated quote
    if (have_token || !cur.empty()) out.push_back(std::move(cur));
  }
  return true;
}

int run_command(RingClient& client, const std::vector<std::string_view>& args) {
  const auto reply = client.command(args);
  if (!reply) {
    std::cerr << "redis-cli-ring: timed out waiting for a reply\n";
    return 1;
  }
  print_reply(*reply, 0, "");
  return 0;
}

// The leading line of a reply, minus its type byte and trailing CRLF -- for
// summarizing an error reply ("-ERR ...") without the framing.
std::string reply_summary(const std::string& reply) {
  const std::size_t eol = reply.find("\r\n");
  const std::size_t end = eol == std::string::npos ? reply.size() : eol;
  return reply.substr(1, end - 1);
}

// Stream a file (or stdin) of commands into the ring: one command per line, each
// pushed as a single atomic record. Blocks on a full ring and throws on an
// over-large command via the ring library; drains replies as it goes so the CQ
// never stalls the server. Prints a redis-cli --pipe-style summary.
int stream_commands(RingClient& client, std::istream& in, const std::string& source) {
  std::size_t line_no = 0, sent = 0, replies = 0, errors = 0, skipped = 0, shown = 0;
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
    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF files
    std::vector<std::string> tokens;
    if (!split_args(line, tokens)) {
      std::cerr << source << ':' << line_no << ": unbalanced quotes -- skipped\n";
      ++skipped;
      continue;
    }
    if (tokens.empty()) continue;  // blank or comment line
    const std::vector<std::string_view> args(tokens.begin(), tokens.end());
    try {
      client.send_command_atomic(args);  // blocks if full; throws if too large
    } catch (const std::length_error& e) {
      std::cerr << source << ':' << line_no << ": command too large for the ring: "
                << e.what() << '\n';
      aborted = true;
      break;
    } catch (const std::exception& e) {
      std::cerr << source << ':' << line_no << ": " << e.what() << '\n';
      aborted = true;
      break;
    }
    ++sent;
    // Drain replies that are already back, so the reply buffer stays bounded and the
    // server never blocks writing into a full CQ.
    while (auto r = client.try_read_reply()) note_reply(*r);
  }

  // Collect the remaining replies -- one per command we streamed.
  while (replies < sent) {
    auto r = client.read_reply();
    if (!r) {
      std::cerr << "  timed out with " << (sent - replies) << " repl(y/ies) outstanding\n";
      break;
    }
    note_reply(*r);
  }

  if (errors > kMaxShown) std::cerr << "  ... and " << (errors - shown) << " more error(s)\n";
  std::cout << "streamed " << sent << " command(s) from " << source << "; " << replies
            << " reply(ies), " << errors << " error(s)";
  if (skipped) std::cout << ", " << skipped << " line(s) skipped";
  if (aborted) std::cout << " (aborted)";
  std::cout << '\n';
  return (aborted || errors > 0) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage:\n"
                 "  redis-cli-ring <ring-path> [COMMAND ARG ...]   one-shot / interactive\n"
                 "  redis-cli-ring <ring-path> -f <file>|-         stream a command file\n";
    return 2;
  }
  const char* path = argv[1];

  // Stream mode: redis-cli-ring <ring> -f <file>|-   ("-" = stdin).
  const bool stream = argc >= 4 && (std::strcmp(argv[2], "-f") == 0 ||
                                    std::strcmp(argv[2], "--file") == 0);

  auto client = RingClient::open(path);
  if (!client) {
    std::cerr << "redis-cli-ring: cannot open ring " << path
              << " (is the server running with --ring " << path << " ...?)\n";
    return 1;
  }

  if (stream) {
    const char* file = argv[3];
    if (std::strcmp(file, "-") == 0) {
      return stream_commands(*client, std::cin, "<stdin>");
    }
    std::ifstream in(file);
    if (!in) {
      std::cerr << "redis-cli-ring: cannot open command file " << file << '\n';
      return 1;
    }
    return stream_commands(*client, in, file);
  }

  if (argc > 2) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    return run_command(*client, args);
  }

  // Interactive: one command per line (quote-aware), a reply printed for each.
  std::string line;
  const bool tty = ::isatty(fileno(stdin)) != 0;
  if (tty) {
    std::cout << "redis-cli-ring " << path << " (one command per line, Ctrl-D to quit)\n";
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
    (void)run_command(*client, args);
  }
  return 0;
}
