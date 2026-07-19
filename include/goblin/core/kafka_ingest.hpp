#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core {

class Store;

struct KafkaConnectionOptions {
  std::string brokers;
  std::string topic;
  std::vector<std::pair<std::string, std::string>> properties;
};

[[nodiscard]] std::optional<KafkaConnectionOptions>
parse_kafka_connection_string(std::string_view connection, std::string& error);

enum class KafkaRecordResult {
  applied,
  filtered,
  error,
};

// Apply exactly one RESP2 array from one Kafka record. Only logical keyspace
// mutations pass the command allowlist. The response is discarded, but a RESP
// error from a write is returned in `error` so replay cannot silently diverge.
[[nodiscard]] KafkaRecordResult apply_kafka_resp2_record(
    Store& store, std::string_view payload, std::string& error);

struct KafkaReplayStats {
  std::size_t records{0};
  std::size_t writes{0};
  std::size_t filtered{0};
};

struct KafkaPollResult {
  KafkaReplayStats stats;
  bool may_have_more{false};
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

class KafkaIngestor {
 public:
  KafkaIngestor(const KafkaIngestor&) = delete;
  KafkaIngestor& operator=(const KafkaIngestor&) = delete;
  KafkaIngestor(KafkaIngestor&&) = delete;
  KafkaIngestor& operator=(KafkaIngestor&&) = delete;
  ~KafkaIngestor();

  [[nodiscard]] static std::unique_ptr<KafkaIngestor> connect(
      std::string_view connection,
      std::optional<std::int64_t> start_timestamp_ms,
      std::string& error);

  // Replay through the high-water marks captured during connect(). This is
  // called before any Goblin listener is created.
  [[nodiscard]] bool catch_up(Store& store, KafkaReplayStats& stats,
                              std::string& error);

  // Nonblocking steady-state drain for the server event loop.
  [[nodiscard]] KafkaPollResult poll(Store& store, std::size_t max_records);

  [[nodiscard]] int notification_fd() const noexcept;
  [[nodiscard]] bool has_pending() const noexcept;
  [[nodiscard]] std::string_view description() const noexcept;

 private:
  struct Impl;
  explicit KafkaIngestor(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace goblin::core
