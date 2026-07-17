// Thin alias: prefer redis-cli-ring --exasock ... for interactive / pipe modes.
// Kept for scripts that already call redis-cli-exasock HOST PORT CMD...

#include "goblin/core/exasock_client.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::uint16_t parse_port(std::string_view text) {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.data(), &end, 10);
  if (end == text.data() || *end != '\0' || value == 0 || value > 65535UL) {
    return 0;
  }
  return static_cast<std::uint16_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr
        << "usage: redis-cli-exasock HOST PORT [COMMAND [ARG ...]]\n"
        << "\n"
        << "Also: redis-cli-ring --exasock HOST PORT [COMMAND ...]\n"
        << "      (interactive / -f pipe modes)\n"
        << "\n"
        << "For SmartNIC acceleration, launch under the exasock wrapper:\n"
        << "  exasock redis-cli-exasock 10.99.99.1 6379 PING\n";
    return 2;
  }

  const std::string_view host = argv[1];
  const auto port = parse_port(argv[2]);
  if (port == 0) {
    std::cerr << "redis-cli-exasock: invalid port\n";
    return 2;
  }

  std::string error;
  goblin::core::exasock::ConnectOptions options;
  options.require_loaded = false;
  auto client = goblin::core::exasock::ExasockClient::open(
      host, port, {}, options, &error);
  if (!client) {
    std::cerr << "redis-cli-exasock: " << error << '\n';
    return 1;
  }

  if (goblin::core::exasock::loaded()) {
    std::cerr << "redis-cli-exasock: ExaSock "
              << goblin::core::exasock::version_text()
              << (client->accelerated() ? " (accelerated)\n"
                                        : " (not accelerated)\n");
  }

  std::vector<std::string_view> args;
  if (argc == 3) {
    args = {"PING"};
  } else {
    args.reserve(static_cast<std::size_t>(argc - 3));
    for (int i = 3; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
  }

  try {
    const auto reply = client->command(args);
    if (!reply) {
      std::cerr << "redis-cli-exasock: timed out waiting for a reply\n";
      return 1;
    }
    std::cout << *reply;
    if (reply->empty() || reply->back() != '\n') {
      std::cout << '\n';
    }
  } catch (const std::exception& ex) {
    std::cerr << "redis-cli-exasock: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
