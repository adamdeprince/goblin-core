#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core {

class Store;
struct Command;

inline constexpr std::uint32_t kReplicationProtocolVersion = 1;

struct ReplicationId {
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::string hex() const;

  friend bool operator==(const ReplicationId&, const ReplicationId&) = default;
};

[[nodiscard]] ReplicationId make_replication_id();
[[nodiscard]] std::optional<ReplicationId> parse_replication_id(
    std::string_view text) noexcept;

// Persisted with the logical keyspace. kafka_acknowledged_offset is the last
// broker offset known to be durable, not the next offset and not the Goblin
// logical offset. Recovery seeks to it inclusively; -1 means that no exact
// Kafka resume point was captured for this snapshot.
struct ReplicationState {
  ReplicationId id{};
  std::uint64_t offset{0};
  std::int64_t kafka_acknowledged_offset{-1};
  bool valid{false};
};

struct ReplicationMutation {
  // Empty means the operation is ordered but cannot be compacted independently.
  std::string kafka_key;
  // Exactly one canonical RESP2 command.
  std::string payload;
};

struct ReplicationBatch {
  ReplicationId id{};
  std::uint64_t offset{0};
  std::vector<ReplicationMutation> mutations;
};

[[nodiscard]] std::string encode_resp2_command(
    std::span<const std::string_view> fields);

// Construct canonical post-state mutations after a successful command. The
// normalized Kafka key uses the smallest independently overwritten identity:
// for example ("ZADD", key, member) and ("HSET", key, field).
[[nodiscard]] std::vector<ReplicationMutation> build_replication_mutations(
    const Store& store, const Command& command, std::string_view response);

// Firehose records are RESP2 arrays so the same bytes can cross TCP, UDS,
// shared-memory rings, and RDMA. Each payload remains an independently valid
// RESP2 write for Kafka and recovery tooling.
[[nodiscard]] std::string encode_firehose_batch(const ReplicationBatch& batch);

// Initial response to GOBLIN.FIREHOSE, before the connection becomes one-way.
[[nodiscard]] std::string encode_firehose_hello(
    const ReplicationState& state);

// Length-framed binary compaction key. Components may contain arbitrary bytes.
[[nodiscard]] std::string make_replication_compaction_key(
    std::span<const std::string_view> components);

}  // namespace goblin::core
