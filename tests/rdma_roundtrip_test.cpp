#include "goblin/core/rdma_client.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

template <class Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "rdma_roundtrip_test: " << message << '\n';
  }
  return condition;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 5) {
    std::cerr << "usage: " << argv[0]
              << " ADDRESS PORT RING-SIZE [PIPELINE-REQUESTS]\n";
    return 2;
  }

  unsigned port_value = 0;
  std::size_t requests = 20'000;
  const auto ring_bytes = goblin::core::ring::parse_size(argv[3]);
  if (!parse_integer<unsigned>(argv[2], port_value) || port_value == 0 ||
      port_value > 65535 || !ring_bytes || *ring_bytes == 0 ||
      (argc == 5 && !parse_integer<std::size_t>(argv[4], requests))) {
    std::cerr << "rdma_roundtrip_test: invalid argument\n";
    return 2;
  }
  const auto port = static_cast<std::uint16_t>(port_value);

  std::string error;
  auto resp = goblin::core::rdma::RdmaClient::open(
      argv[1], port, *ring_bytes, std::chrono::seconds(5), &error);
  if (!resp) {
    std::cerr << "rdma_roundtrip_test: RESP connect: " << error << '\n';
    return 1;
  }
  if (!expect(resp->command({"PING"}) == "+PONG\r\n", "RESP PING failed")) {
    return 1;
  }

  const std::string large_value(8192, 'g');
  if (!expect(resp->command({"SET", "rdma-large", large_value}) == "+OK\r\n",
              "fragmented RESP SET failed")) {
    return 1;
  }
  const std::string expected_large =
      "$8192\r\n" + large_value + "\r\n";
  if (!expect(resp->command({"GET", "rdma-large"}) == expected_large,
              "fragmented RESP GET failed")) {
    return 1;
  }
  const auto hello = resp->command({"HELLO", "3"});
  if (!expect(hello && !hello->empty() && (*hello)[0] == '%',
              "RESP3 HELLO failed over RDMA")) {
    return 1;
  }

  constexpr std::size_t kBatch = 200;
  const std::array<std::string_view, 1> ping{"PING"};
  std::size_t completed = 0;
  while (completed < requests) {
    const std::size_t batch = std::min(kBatch, requests - completed);
    for (std::size_t i = 0; i < batch; ++i) {
      resp->send_pipelined(ping);
    }
    for (std::size_t i = 0; i < batch; ++i) {
      if (!expect(resp->read_reply() == "+PONG\r\n",
                  "pipelined RESP PING failed")) {
        return 1;
      }
    }
    completed += batch;
  }

  error.clear();
  auto sbe = goblin::core::SbeRdmaClient::open(
      std::string_view(argv[1]), port, *ring_bytes, std::chrono::seconds(5),
      16 * 1024, &error);
  if (!sbe) {
    std::cerr << "rdma_roundtrip_test: SBE connect: " << error << '\n';
    return 1;
  }
  if (!expect(sbe->ping(), "SBE PING failed")) {
    return 1;
  }
  const auto set = sbe->set("rdma-sbe", large_value);
  if (!expect(set.ok, "fragmented SBE SET failed") ||
      !expect(sbe->get("rdma-sbe") == large_value,
              "fragmented SBE GET failed")) {
    return 1;
  }

  for (unsigned i = 0; i < 64; ++i) {
    error.clear();
    auto churn = goblin::core::rdma::RdmaClient::open(
        argv[1], port, *ring_bytes, std::chrono::seconds(5), &error);
    if (!churn || churn->command({"PING"}) != "+PONG\r\n") {
      std::cerr << "rdma_roundtrip_test: connection churn failed at " << i
                << ": " << error << '\n';
      return 1;
    }
  }

  std::cout << "RDMA RESP/SBE round trip passed: " << requests
            << " pipelined requests, fragmented 8 KiB values, and 64 reconnects\n";
  return 0;
}
