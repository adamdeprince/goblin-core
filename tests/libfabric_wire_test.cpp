#include "goblin/core/libfabric_wire.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace goblin::core::libfabric_wire;

namespace {

void header_round_trip() {
  std::array<std::byte, kHeaderBytes + 3> wire{};
  Header source{
      .kind = Kind::data,
      .flags = 7,
      .payload_bytes = 3,
      .session_id = 0x1020304050607080ULL,
      .sequence = 42,
      .reply_to = 41,
  };
  assert(encode_header(wire, source));
  const auto decoded = decode_header(wire);
  assert(decoded);
  assert(decoded->kind == source.kind);
  assert(decoded->flags == source.flags);
  assert(decoded->payload_bytes == source.payload_bytes);
  assert(decoded->session_id == source.session_id);
  assert(decoded->sequence == source.sequence);
  assert(decoded->reply_to == source.reply_to);

  wire[0] = std::byte{0};
  assert(!decode_header(wire));
}

void hello_round_trip() {
  constexpr std::string_view version = "0.10.2";
  const std::array address{std::byte{1}, std::byte{2}, std::byte{3}};
  std::vector<std::byte> wire(hello_payload_bytes(version, address.size()));
  assert(encode_hello(wire, version, address, 64));
  const auto decoded = decode_hello(wire);
  assert(decoded);
  assert(decoded->goblin_version == version);
  assert(decoded->reorder_messages == 64);
  assert(std::equal(decoded->endpoint_address.begin(),
                    decoded->endpoint_address.end(), address.begin()));

  std::array<std::byte, kHelloAckBytes> ack_wire{};
  const HelloAck ack{.heartbeat_timeout_ms = 3000,
                     .reorder_messages = 64,
                     .max_payload_bytes = 65536};
  assert(encode_hello_ack(ack_wire, ack));
  const auto decoded_ack = decode_hello_ack(ack_wire);
  assert(decoded_ack);
  assert(decoded_ack->heartbeat_timeout_ms == 3000);
  assert(decoded_ack->reorder_messages == 64);
  assert(decoded_ack->max_payload_bytes == 65536);
}

void ordered_delivery() {
  OrderedInbox<std::string> inbox(8, 1024);
  std::vector<std::pair<std::uint64_t, std::string>> delivered;
  const auto deliver = [&](std::string value, std::uint64_t sequence) {
    delivered.emplace_back(sequence, std::move(value));
  };

  assert(inbox.push(2, "two", 3, deliver) == OrderResult::sequestered);
  assert(inbox.next_expected() == 1);
  assert(inbox.push(4, "four", 4, deliver) == OrderResult::sequestered);
  assert(inbox.push(1, "one", 3, deliver) == OrderResult::delivered);
  assert(inbox.next_expected() == 3);
  assert(inbox.push(3, "three", 5, deliver) == OrderResult::delivered);
  assert(inbox.next_expected() == 5);
  assert(inbox.sequestered_messages() == 0);

  assert(delivered.size() == 4);
  for (std::size_t i = 0; i < delivered.size(); ++i) {
    assert(delivered[i].first == i + 1);
  }
  assert(inbox.push(3, "duplicate", 9, deliver) == OrderResult::duplicate);
  assert(inbox.push(20, "too-far", 7, deliver) == OrderResult::overflow);
}

}  // namespace

int main() {
  header_round_trip();
  hello_round_trip();
  ordered_delivery();
  return 0;
}
