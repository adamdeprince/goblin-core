// redis-cli-ring: a proof-of-concept client that talks to goblin-core over a
// shared-memory ring buffer instead of a socket. It shows the whole client side
// of the protocol -- open the ring, encode a RESP command, push it on the SQ,
// then busy-poll the CQ for the reply -- in a few dozen lines, all on top of the
// header-only client in goblin/core/ring_client.hpp.
//
//   redis-cli-ring /tmp/a SET foo bar      # one-shot: run one command, print reply
//   redis-cli-ring /tmp/a                  # interactive: a command per stdin line
//
// The server must already be running with a matching `--ring /tmp/a <size>`.

#include "goblin/core/ring_client.hpp"

#include <charconv>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using goblin::core::ring::RingClient;

// Pretty-print one complete RESP2 reply, redis-cli style. Returns the number of
// bytes consumed so arrays can print their elements in sequence.
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
    case '$': {
      const long long len = to_ll(line);
      if (len < 0) {
        std::cout << "(nil)\n";
        return after;
      }
      std::cout << '"' << s.substr(after, static_cast<std::size_t>(len)) << "\"\n";
      return after + static_cast<std::size_t>(len) + 2;
    }
    case '*': {
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
    default:
      std::cout << line << '\n';
      return after;
  }
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: redis-cli-ring <ring-path> [COMMAND ARG ...]\n";
    return 2;
  }
  const char* path = argv[1];
  auto client = RingClient::open(path);
  if (!client) {
    std::cerr << "redis-cli-ring: cannot open ring " << path
              << " (is the server running with --ring " << path << " ...?)\n";
    return 1;
  }

  if (argc > 2) {
    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    return run_command(*client, args);
  }

  // Interactive: one command per line. Whitespace-split only (no quoting) -- this
  // is a proof of concept, not a full shell.
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
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
      tokens.push_back(token);
    }
    if (tokens.empty()) {
      continue;
    }
    std::vector<std::string_view> args(tokens.begin(), tokens.end());
    (void)run_command(*client, args);
  }
  return 0;
}
