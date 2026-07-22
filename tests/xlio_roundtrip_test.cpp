// Live-fabric RESP and SBE interoperability test for the native XLIO Ultra
// transport. Start an XLIO-enabled Goblin server separately, then run this
// binary under the pinned libxlio preload on the peer host.

#include "goblin/core/sbe_ring_client.hpp"
#include "goblin/core/xlio_client.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr,
                 "usage: %s HOST PORT [LOCAL-ADDRESS]\n",
                 argv[0]);
    return 2;
  }
  unsigned long port_value = 0;
  try {
    port_value = std::stoul(argv[2]);
  } catch (...) {
    std::fprintf(stderr, "xlio_roundtrip_test: invalid port\n");
    return 2;
  }
  if (port_value == 0 || port_value > 65535) {
    std::fprintf(stderr, "xlio_roundtrip_test: invalid port\n");
    return 2;
  }

  const auto port = static_cast<std::uint16_t>(port_value);
  const std::string_view local_address = argc == 4 ? argv[3] : "";
  std::string error;
  auto resp = goblin::core::xlio::XlioClient::open(
      argv[1], port, std::chrono::seconds(5), local_address, &error);
  if (!resp) {
    std::fprintf(stderr, "xlio_roundtrip_test: RESP connect: %s\n",
                 error.c_str());
    return 1;
  }
  if (resp->command({"PING"}) != "+PONG\r\n" ||
      resp->command({"SET", "xlio:resp:key", "hello"}) != "+OK\r\n" ||
      resp->command({"GET", "xlio:resp:key"}) != "$5\r\nhello\r\n") {
    std::fprintf(stderr, "xlio_roundtrip_test: RESP round trip failed\n");
    return 1;
  }
  const auto info = resp->command({"INFO"});
  if (!info || info->find("xlio_support:1\r\n") == std::string::npos ||
      info->find("xlio_loaded:1\r\n") == std::string::npos) {
    std::fprintf(stderr, "xlio_roundtrip_test: INFO lacks XLIO runtime state\n");
    return 1;
  }

  auto sbe = goblin::core::SbeXlioClient::open(
      std::string_view(argv[1]), port, std::chrono::seconds(5), local_address,
      &error);
  if (!sbe) {
    std::fprintf(stderr, "xlio_roundtrip_test: SBE connect: %s\n",
                 error.c_str());
    return 1;
  }
  if (!sbe->ping() || !sbe->set("xlio:sbe:key", "typed").ok ||
      sbe->get("xlio:sbe:key") != "typed") {
    std::fprintf(stderr, "xlio_roundtrip_test: SBE round trip failed\n");
    return 1;
  }

  std::puts("XLIO Ultra RESP + SBE round trip OK");
  return 0;
}
