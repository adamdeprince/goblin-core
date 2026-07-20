#pragma once

#include "goblin/core/replication.hpp"
#include "goblin/core/server.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace goblin::core {

[[nodiscard]] std::string describe_replica_source(
    const ReplicaSourceConfig& config);

class ReplicationFollowerRuntime {
 public:
  ReplicationFollowerRuntime(const ReplicaSourceConfig& config,
                             std::size_t buffer_bytes,
                             const ReplicaAuthConfig* auth = nullptr,
                             std::chrono::milliseconds connect_timeout =
                                 std::chrono::seconds(5));
  ~ReplicationFollowerRuntime();

  ReplicationFollowerRuntime(const ReplicationFollowerRuntime&) = delete;
  ReplicationFollowerRuntime& operator=(const ReplicationFollowerRuntime&) =
      delete;

  [[nodiscard]] const FirehoseHello& hello() const noexcept;
  [[nodiscard]] const std::string& description() const noexcept;
  [[nodiscard]] bool polled() const noexcept;
  [[nodiscard]] int notification_fd() const noexcept;
  [[nodiscard]] bool has_buffered() const noexcept;

  // Drain every currently available live frame into the fixed mmap buffer.
  // Used while Kafka catches up to the offset captured by hello().
  [[nodiscard]] bool buffer_available(std::string& error);

  // Return one decoded batch, preferring the startup buffer. `available` is
  // false when the transport is merely idle; transport/protocol failures set
  // error and return false.
  [[nodiscard]] bool try_next(ReplicationBatch& batch, bool& available,
                              std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace goblin::core
