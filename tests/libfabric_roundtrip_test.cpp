#include "goblin/core/sbe_ring_client.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <class Integer>
[[nodiscard]] bool parse_integer(std::string_view text, Integer& value) {
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "libfabric_roundtrip_test: " << message << '\n';
  }
  return condition;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || argc > 7) {
    std::cerr
        << "usage: " << argv[0]
        << " PROVIDER HOST PORT [LOCAL-ADDRESS|-] [PIPELINE-REQUESTS]"
           " [IDLE-MS]\n";
    return 2;
  }

  unsigned port_value = 0;
  std::size_t requests = 2000;
  std::uint64_t idle_ms = 0;
  if (!parse_integer<unsigned>(argv[3], port_value) || port_value == 0 ||
      port_value > 65535 ||
      (argc >= 6 && !parse_integer<std::size_t>(argv[5], requests)) ||
      (argc == 7 && !parse_integer<std::uint64_t>(argv[6], idle_ms))) {
    std::cerr << "libfabric_roundtrip_test: invalid numeric argument\n";
    return 2;
  }

  const std::string_view provider = argv[1];
  const std::string_view host = argv[2];
  const std::string_view local_address =
      argc >= 5 && std::string_view(argv[4]) != "-" ? argv[4] : "";
  const auto port = static_cast<std::uint16_t>(port_value);
  constexpr std::size_t kBufferBytes = 128U * 1024U;

  const auto open_client = [&](std::string& error) {
    return goblin::core::SbeLibfabricClient::open(
        provider, host, port, std::chrono::seconds(5), kBufferBytes,
        local_address, &error);
  };

  std::string error;
  auto client = open_client(error);
  if (!client) {
    std::cerr << "libfabric_roundtrip_test: primary connect: " << error
              << '\n';
    return 1;
  }
  if (!expect(client->ping(), "SBE PING failed")) return 1;

  const std::string large_value(8192, 'f');
  if (!expect(client->set("libfabric:large", large_value).ok,
              "SBE SET failed") ||
      !expect(client->get("libfabric:large") == large_value,
              "SBE GET failed")) {
    return 1;
  }

  const std::string hash_key =
      "libfabric:pipeline:" +
      std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count());
  std::vector<std::string> fields;
  std::vector<std::string> values;
  fields.reserve(requests);
  values.reserve(requests);
  for (std::size_t index = 0; index < requests; ++index) {
    fields.push_back("field-" + std::to_string(index));
    values.push_back("value-" + std::to_string(index));
  }

  constexpr std::size_t kPipelineDepth = 64;
  client->pipeline_for(
      requests, kPipelineDepth,
      [&](std::size_t index) {
        const std::array<std::pair<std::string_view, std::string_view>, 1>
            entry{{{fields[index], values[index]}}};
        client->enqueue_hset(hash_key, entry);
      },
      [&](std::size_t) {
        if (client->read_pipeline_int() != 1) {
          throw std::runtime_error("pipelined HSET did not add its field");
        }
      });

  client->pipeline_for(
      requests, kPipelineDepth,
      [&](std::size_t index) {
        client->enqueue_hget(hash_key, fields[index]);
      },
      [&](std::size_t index) {
        const auto value = client->read_pipeline_bulk_or_nil();
        if (!value || *value != values[index]) {
          throw std::runtime_error("pipelined HGET returned the wrong value");
        }
      });

  error.clear();
  auto subscriber = open_client(error);
  if (!subscriber) {
    std::cerr << "libfabric_roundtrip_test: subscriber connect: " << error
              << '\n';
    return 1;
  }
  constexpr std::string_view kChannel = "libfabric:qualification";
  constexpr std::string_view kPayload = "ordered-push";
  const std::array<std::string_view, 1> channels{kChannel};
  const auto acknowledgements = subscriber->subscribe(channels);
  if (!expect(acknowledgements.size() == 1,
              "SBE SUBSCRIBE acknowledgement failed") ||
      !expect(client->publish(kChannel, kPayload) == 1,
              "SBE PUBLISH subscriber count was not one")) {
    return 1;
  }
  const auto message = subscriber->read_pubsub();
  if (!expect(message.kind == goblin::core::PubSubKind::message,
              "unexpected Pub/Sub push kind") ||
      !expect(message.channel == kChannel, "Pub/Sub channel mismatch") ||
      !expect(message.payload == kPayload, "Pub/Sub payload mismatch")) {
    return 1;
  }

  if (idle_ms != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    if (!expect(client->ping(), "session expired despite automatic heartbeat") ||
        !expect(subscriber->ping(),
                "subscriber expired despite automatic heartbeat")) {
      return 1;
    }
  }

  std::cout << "libfabric " << provider
            << " round trip passed: two clients, " << requests
            << " pipelined HSET/HGET pairs, Pub/Sub, and " << idle_ms
            << " ms idle heartbeat interval\n";
  return 0;
}
