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

// Process-local follower health. Unlike ReplicationState, this is never saved:
// it describes the current upstream connection and recovery attempt, not the
// durable lineage boundary represented by the keyspace.
enum class ReplicaSyncState : std::uint8_t {
  disabled,
  connecting,
  replaying_kafka,
  buffering_firehose,
  live,
  reconnecting,
  degraded,
};

[[nodiscard]] constexpr std::string_view replica_sync_state_name(
    ReplicaSyncState state) noexcept {
  switch (state) {
    case ReplicaSyncState::disabled:
      return "disabled";
    case ReplicaSyncState::connecting:
      return "connecting";
    case ReplicaSyncState::replaying_kafka:
      return "replaying_kafka";
    case ReplicaSyncState::buffering_firehose:
      return "buffering_firehose";
    case ReplicaSyncState::live:
      return "live";
    case ReplicaSyncState::reconnecting:
      return "reconnecting";
    case ReplicaSyncState::degraded:
      return "degraded";
  }
  return "degraded";
}

struct ReplicaRuntimeStatus {
  ReplicaSyncState state{ReplicaSyncState::disabled};
  std::uint64_t upstream_offset{0};
  std::uint64_t reconnect_attempts{0};
  std::uint64_t successful_reconnects{0};
  std::uint64_t last_io_unix_ms{0};
  std::string last_error;

  [[nodiscard]] bool ready() const noexcept {
    return state == ReplicaSyncState::live;
  }
};

struct ReplicationMutation {
  // Empty means the operation is ordered but cannot be compacted independently.
  std::string kafka_key;
  // Exactly one canonical RESP2 command.
  std::string payload;
};

struct ReplicationBatch {
  ReplicationId id{};
  // Logical offset of mutations.front(); later mutations are consecutive.
  std::uint64_t offset{0};
  std::vector<ReplicationMutation> mutations;
};

struct FirehoseHello {
  ReplicationId id{};
  std::uint64_t offset{0};
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

// Decode exactly one transport-neutral RESP2 firehose frame. Payloads are
// copied because the returned object must outlive the parser's input buffer.
[[nodiscard]] std::optional<FirehoseHello> decode_firehose_hello(
    std::string_view bytes, std::string& error);
[[nodiscard]] std::optional<ReplicationBatch> decode_firehose_batch(
    std::string_view bytes, std::string& error);

// Apply the non-duplicate suffix of one live batch. Unlike compacted Kafka
// replay, a firehose is contiguous: a missing logical offset is fatal.
[[nodiscard]] bool apply_firehose_batch(Store& store,
                                        const ReplicationBatch& batch,
                                        std::string& error);

// Length-framed binary compaction key. Components may contain arbitrary bytes.
[[nodiscard]] std::string make_replication_compaction_key(
    std::span<const std::string_view> components);

}  // namespace goblin::core
