// Thin alias: prefer redis-cli-ring --rdma ... for interactive / pipe modes.
// Kept for scripts that already call redis-cli-rdma HOST PORT SIZE CMD...

#include "goblin/core/rdma_client.hpp"
#include "goblin/core/ring_buffer.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] bool parse_port(std::string_view text, std::uint16_t& port) {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
      value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "usage: " << argv[0]
              << " ADDRESS PORT RING-SIZE COMMAND [ARG ...]\n"
              << "\n"
              << "Also: redis-cli-ring --rdma ADDRESS PORT SIZE [COMMAND ...]\n"
              << "      (interactive / -f pipe modes)\n";
    return 2;
  }

  std::uint16_t port = 0;
  const auto ring_bytes = goblin::core::ring::parse_size(argv[3]);
  if (!parse_port(argv[2], port) || !ring_bytes || *ring_bytes == 0) {
    std::cerr << "redis-cli-rdma: invalid port or ring size\n";
    return 2;
  }

  std::string error;
  auto client = goblin::core::rdma::RdmaClient::open(
      argv[1], port, *ring_bytes, std::chrono::seconds(5), &error);
  if (!client) {
    std::cerr << "redis-cli-rdma: " << error << '\n';
    return 1;
  }

  std::vector<std::string_view> command;
  command.reserve(static_cast<std::size_t>(argc - 4));
  for (int i = 4; i < argc; ++i) {
    command.emplace_back(argv[i]);
  }

  try {
    const auto reply = client->command(command);
    if (!reply) {
      std::cerr << "redis-cli-rdma: timed out waiting for a reply\n";
      return 1;
    }
    std::cout.write(reply->data(), static_cast<std::streamsize>(reply->size()));
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
