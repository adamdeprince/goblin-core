#include "goblin/core/kafka_ingest.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/store.hpp"
#include "socket_test_utils.hpp"

#include <rdkafka.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

struct DeliveryState {
  std::atomic_size_t complete{0};
  std::atomic_bool failed{false};
  std::atomic<std::int64_t> last_offset{-1};
};

void delivery_report(rd_kafka_t*, const rd_kafka_message_t* message,
                     void* opaque) {
  auto& state = *static_cast<DeliveryState*>(opaque);
  if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
    state.failed = true;
  } else {
    state.last_offset = message->offset;
  }
  ++state.complete;
}

class Producer {
 public:
  Producer(std::string_view brokers, std::string_view topic)
      : topic_(topic) {
    rd_kafka_conf_t* config = rd_kafka_conf_new();
    char error[512]{};
    assert(rd_kafka_conf_set(config, "bootstrap.servers",
                             std::string(brokers).c_str(), error,
                             sizeof(error)) == RD_KAFKA_CONF_OK);
    rd_kafka_conf_set_opaque(config, &delivery_);
    rd_kafka_conf_set_dr_msg_cb(config, delivery_report);
    producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, config, error, sizeof(error));
    assert(producer_ != nullptr);
  }

  ~Producer() {
    if (producer_ != nullptr) rd_kafka_destroy(producer_);
  }

  std::int64_t send(std::initializer_list<std::string_view> fields,
                    std::int32_t partition = 0) {
    const auto payload = encode_command(
        std::span<const std::string_view>(fields.begin(), fields.size()));
    return send_payload(payload, partition, nullptr, 0);
  }

  std::int64_t send_replication(
      std::initializer_list<std::string_view> fields,
      const goblin::core::ReplicationId& id, std::uint64_t logical_offset) {
    const auto payload = encode_command(
        std::span<const std::string_view>(fields.begin(), fields.size()));
    return send_payload(payload, 0, &id, logical_offset);
  }

 private:
  std::int64_t send_payload(std::string_view payload, std::int32_t partition,
                            const goblin::core::ReplicationId* id,
                            std::uint64_t logical_offset) {
    const auto expected = delivery_.complete.load() + 1;
    rd_kafka_resp_err_t error = RD_KAFKA_RESP_ERR_NO_ERROR;
    if (id == nullptr) {
      error = rd_kafka_producev(
          producer_, RD_KAFKA_V_TOPIC(topic_.c_str()),
          RD_KAFKA_V_PARTITION(partition),
          RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
          RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
          RD_KAFKA_V_END);
    } else {
      const std::string version =
          std::to_string(goblin::core::kReplicationProtocolVersion);
      const std::string id_text = id->hex();
      const std::string offset_text = std::to_string(logical_offset);
      error = rd_kafka_producev(
          producer_, RD_KAFKA_V_TOPIC(topic_.c_str()),
          RD_KAFKA_V_PARTITION(partition),
          RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
          RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
          RD_KAFKA_V_HEADER(
              goblin::core::kKafkaReplicationVersionHeader.data(),
              version.data(), version.size()),
          RD_KAFKA_V_HEADER(goblin::core::kKafkaReplicationIdHeader.data(),
                            id_text.data(), id_text.size()),
          RD_KAFKA_V_HEADER(goblin::core::kKafkaReplicationOffsetHeader.data(),
                            offset_text.data(), offset_text.size()),
          RD_KAFKA_V_END);
    }
    assert(error == RD_KAFKA_RESP_ERR_NO_ERROR);
    while (delivery_.complete.load() < expected) rd_kafka_poll(producer_, 100);
    assert(!delivery_.failed.load());
    return delivery_.last_offset.load();
  }

  rd_kafka_t* producer_{nullptr};
  std::string topic_;
  DeliveryState delivery_;
};

int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  assert(path.size() < sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int wait_for_server(const std::string& path) {
  for (int attempt = 0; attempt < 1500; ++attempt) {
    if (const int fd = connect_uds(path); fd >= 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

void send_command(int fd, std::initializer_list<std::string_view> fields) {
  const auto payload = encode_command(
      std::span<const std::string_view>(fields.begin(), fields.size()));
  std::size_t offset = 0;
  while (offset != payload.size()) {
    const auto sent =
        ::send(fd, payload.data() + offset, payload.size() - offset, 0);
    assert(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

std::string read_reply(int fd, std::string& pending) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    if (const auto end = reply_end(pending)) {
      auto reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char buffer[4096];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

std::int64_t info_integer(std::string_view reply, std::string_view name) {
  const std::string needle = std::string(name) + ':';
  const auto begin_offset = reply.find(needle);
  assert(begin_offset != std::string_view::npos);
  const auto begin = reply.data() + begin_offset + needle.size();
  const auto line_end = reply.find("\r\n", begin_offset + needle.size());
  assert(line_end != std::string_view::npos);
  const auto end = reply.data() + line_end;
  std::int64_t value = 0;
  const auto [parsed, error] = std::from_chars(begin, end, value);
  assert(error == std::errc{} && parsed == end);
  return value;
}

struct Child {
  pid_t pid{-1};
  ~Child() { stop(); }
  void stop() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
};

Child spawn_server(const char* binary, std::vector<std::string> arguments) {
  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(binary));
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(binary, argv.data());
    _exit(127);
  }
  return Child{.pid = pid};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: kafka_server_test <goblin-core> <brokers> <topic>\n";
    return 2;
  }

  const std::string socket =
      "/tmp/goblin-kafka-test-" + std::to_string(::getpid()) + ".sock";
  const std::string snapshot =
      "/tmp/goblin-kafka-test-" + std::to_string(::getpid()) + ".gcsn";
  const std::string recovery_snapshot = snapshot + ".recovery";
  const std::string live_snapshot = snapshot + ".live";
  const std::string ahead_snapshot = snapshot + ".ahead";
  const std::string replica_socket = socket + ".replica";
  (void)::unlink(socket.c_str());
  (void)::unlink(replica_socket.c_str());
  (void)::unlink(snapshot.c_str());
  (void)::unlink(recovery_snapshot.c_str());
  (void)::unlink(live_snapshot.c_str());
  (void)::unlink(ahead_snapshot.c_str());
  Producer producer(argv[2], argv[3]);
  producer.send({"SET", "startup-key", "from-backlog"}, 0);
  producer.send({"GET", "startup-key"}, 0);
  producer.send({"HSET", "startup-hash", "field", "value"}, 0);

  const std::string connection =
      "kafka://" + std::string(argv[2]) + '/' + argv[3];
  const std::string broker_ack_connection = connection + "?linger.ms=500";
  {
    const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
    assert(tcp_port != 0);
    auto server = spawn_server(
        argv[1], {"--uds-listen", socket, "--port", std::to_string(tcp_port),
                  "--kafka", broker_ack_connection, "--kafka-ack-mode",
                  "broker", "--kafka-pending-bytes", "1mb"});
    const int client = wait_for_server(socket);
    assert(client >= 0);
    std::string pending;

    send_command(client, {"GET", "startup-key"});
    assert(read_reply(client, pending) == "$12\r\nfrom-backlog\r\n");
    send_command(client, {"HGET", "startup-hash", "field"});
    assert(read_reply(client, pending) == "$5\r\nvalue\r\n");

    producer.send({"SET", "runtime-key", "streamed"}, 0);
    bool observed = false;
    for (int attempt = 0; attempt < 500 && !observed; ++attempt) {
      send_command(client, {"GET", "runtime-key"});
      observed = read_reply(client, pending) == "$8\r\nstreamed\r\n";
      if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(observed);

    send_command(client, {"INFO"});
    const auto before_info = read_reply(client, pending);
    const auto before_broker_offset =
        info_integer(before_info, "kafka_acknowledged_offset");
    assert(info_integer(before_info, "kafka_acknowledged_logical_offset") ==
           info_integer(before_info, "master_repl_offset"));

    send_command(client, {"SET", "journaled-key", "from-client"});
    pollfd early_reply{.fd = client, .events = POLLIN, .revents = 0};
    assert(::poll(&early_reply, 1, 100) == 0);
    assert(read_reply(client, pending) == "+OK\r\n");

    // Broker mode releases the reply only from the delivery callback. The next
    // command therefore observes both the logical and physical broker cursors.
    send_command(client, {"INFO"});
    const auto after_info = read_reply(client, pending);
    assert(info_integer(after_info, "kafka_acknowledged_offset") >
           before_broker_offset);
    assert(info_integer(after_info, "kafka_acknowledged_logical_offset") ==
           info_integer(after_info, "master_repl_offset"));
    assert(info_integer(after_info, "kafka_pending_records") == 0);

    send_command(client, {"MULTI"});
    assert(read_reply(client, pending) == "+OK\r\n");
    send_command(client, {"HSET", "atomic-hash", "one", "1", "two", "2"});
    assert(read_reply(client, pending) == "+QUEUED\r\n");
    send_command(client, {"SET", "atomic-string", "present"});
    assert(read_reply(client, pending) == "+QUEUED\r\n");
    send_command(client, {"EXEC"});
    early_reply = pollfd{.fd = client, .events = POLLIN, .revents = 0};
    assert(::poll(&early_reply, 1, 100) == 0);
    assert(read_reply(client, pending) == "*2\r\n:2\r\n+OK\r\n");
    send_command(client, {"INFO"});
    const auto transaction_info = read_reply(client, pending);
    assert(info_integer(transaction_info,
                        "kafka_acknowledged_logical_offset") ==
           info_integer(transaction_info, "master_repl_offset"));
    assert(info_integer(transaction_info, "kafka_pending_records") == 0);

    send_command(client, {"GOBLIN.SAVE", snapshot, "NOACCEL"});
    assert(read_reply(client, pending) == "+Background saving started\r\n");
    for (int attempt = 0; attempt < 500 && !std::filesystem::exists(snapshot);
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(std::filesystem::exists(snapshot));
    ::close(client);
  }

  (void)::unlink(socket.c_str());
  goblin::core::Store snapshot_store;
  {
    std::ifstream input(snapshot, std::ios::binary);
    assert(input);
    (void)snapshot_store.load(input);
  }
  const auto saved_state = snapshot_store.replication_state();
  assert(saved_state.valid);
  assert(saved_state.offset > 0);
  assert(saved_state.kafka_acknowledged_offset >= 0);
  assert(snapshot_store.get("journaled-key") ==
         std::optional<std::string_view>{"from-client"});

  // Make the saved broker cursor deliberately conservative. Recovery starts on
  // an older lineage, skips a matching duplicate, then applies the first later
  // record from the snapshot's lineage.
  const auto old_lineage = goblin::core::make_replication_id();
  const auto foreign_offset = producer.send_replication(
      {"SET", "foreign-prefix", "discard-me"}, old_lineage, 1);
  snapshot_store.set_kafka_acknowledged_offset(foreign_offset);
  {
    std::ofstream output(recovery_snapshot, std::ios::binary);
    assert(output);
    snapshot_store.save(output, false);
    output.close();
    assert(output);
  }
  producer.send_replication({"SET", "foreign-prefix-2", "discard-me"},
                            old_lineage, 2);
  producer.send_replication({"INCR", "journaled-key"}, saved_state.id,
                            saved_state.offset);
  producer.send_replication({"SET", "cutoff-key", "after-snapshot"},
                            saved_state.id, saved_state.offset + 1);

  {
    const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
    assert(tcp_port != 0);
    auto server = spawn_server(
        argv[1], {"--uds-listen", socket, "--port", std::to_string(tcp_port),
                  "--load", recovery_snapshot,
                  "--kafka-time-buffer", "0", "--kafka", connection});
    const int client = wait_for_server(socket);
    assert(client >= 0);
    std::string pending;
    send_command(client, {"GET", "cutoff-key"});
    assert(read_reply(client, pending) == "$14\r\nafter-snapshot\r\n");
    send_command(client, {"GET", "foreign-prefix"});
    assert(read_reply(client, pending) == "$-1\r\n");
    send_command(client, {"GET", "foreign-prefix-2"});
    assert(read_reply(client, pending) == "$-1\r\n");
    send_command(client, {"GET", "journaled-key"});
    assert(read_reply(client, pending) == "$11\r\nfrom-client\r\n");
    ::close(client);
  }

  // Recover both sides from the same snapshot/Kafka boundary. The replica
  // connects its firehose first, catches Kafka up to the hello offset, and then
  // must receive the next primary write over the live stream.
  (void)::unlink(socket.c_str());
  {
    const auto primary_port = goblin::test::reserve_loopback_tcp_port();
    const auto replica_port = goblin::test::reserve_loopback_tcp_port();
    assert(primary_port != 0 && replica_port != 0 &&
           primary_port != replica_port);
    auto primary = spawn_server(
        argv[1], {"--uds-listen", socket, "--port",
                  std::to_string(primary_port), "--load", recovery_snapshot,
                  "--kafka", connection});
    const int primary_client = wait_for_server(socket);
    assert(primary_client >= 0);

    auto replica = spawn_server(
        argv[1], {"--uds-listen", replica_socket, "--port",
                  std::to_string(replica_port), "--load", recovery_snapshot,
                  "--kafka", connection, "--replica-uds", socket});
    const int replica_client = wait_for_server(replica_socket);
    assert(replica_client >= 0);

    std::string primary_pending;
    std::string replica_pending;
    send_command(replica_client, {"GET", "cutoff-key"});
    assert(read_reply(replica_client, replica_pending) ==
           "$14\r\nafter-snapshot\r\n");
    send_command(replica_client, {"ROLE"});
    assert(read_reply(replica_client, replica_pending).find("Unix socket") !=
           std::string::npos);

    send_command(primary_client, {"SET", "after-handoff", "from-firehose"});
    assert(read_reply(primary_client, primary_pending) == "+OK\r\n");
    bool observed = false;
    for (int attempt = 0; attempt < 500 && !observed; ++attempt) {
      send_command(replica_client, {"GET", "after-handoff"});
      observed = read_reply(replica_client, replica_pending) ==
                 "$13\r\nfrom-firehose\r\n";
      if (!observed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(observed);
    send_command(replica_client, {"SET", "replica-write", "rejected"});
    assert(read_reply(replica_client, replica_pending).starts_with(
        "-READONLY "));

    // Capture the live primary, then restart it one logical mutation ahead of
    // the still-running replica. The same mutation is placed in Kafka. Runtime
    // recovery must replay it inclusively, filter any older duplicates, drain
    // the firehose suffix, and return to ready without restarting the replica.
    send_command(primary_client, {"GOBLIN.SAVE", live_snapshot, "NOACCEL"});
    assert(read_reply(primary_client, primary_pending) ==
           "+Background saving started\r\n");
    for (int attempt = 0;
         attempt < 1000 && !std::filesystem::exists(live_snapshot);
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(std::filesystem::exists(live_snapshot));

    goblin::core::Store ahead;
    {
      std::ifstream input(live_snapshot, std::ios::binary);
      assert(input);
      (void)ahead.load(input);
    }
    const auto before_gap = ahead.replication_state();
    assert(before_gap.valid);
    const auto gap_offset = before_gap.offset + 1;
    producer.send_replication({"SET", "runtime-gap", "from-kafka"},
                              before_gap.id, gap_offset);
    std::string apply_error;
    const auto gap_payload = encode_command(
        std::array<std::string_view, 3>{"SET", "runtime-gap", "from-kafka"});
    assert(apply_kafka_replication_record(ahead, before_gap.id, gap_offset,
                                          gap_payload, apply_error) ==
           goblin::core::KafkaRecordResult::applied);
    {
      std::ofstream output(ahead_snapshot, std::ios::binary);
      assert(output);
      ahead.save(output, false);
      output.close();
      assert(output);
    }

    primary.stop();
    ::close(primary_client);
    bool unready = false;
    for (int attempt = 0; attempt < 1000 && !unready; ++attempt) {
      send_command(replica_client, {"INFO"});
      unready = read_reply(replica_client, replica_pending)
                    .find("goblin_ready:0\r\n") != std::string::npos;
      if (!unready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(unready);

    auto restarted_primary = spawn_server(
        argv[1], {"--uds-listen", socket, "--port",
                  std::to_string(primary_port), "--load", ahead_snapshot,
                  "--kafka", connection});
    const int restarted_primary_client = wait_for_server(socket);
    assert(restarted_primary_client >= 0);
    bool recovered = false;
    for (int attempt = 0; attempt < 1500 && !recovered; ++attempt) {
      send_command(replica_client, {"GET", "runtime-gap"});
      recovered = read_reply(replica_client, replica_pending) ==
                  "$10\r\nfrom-kafka\r\n";
      if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(recovered);
    send_command(replica_client, {"INFO"});
    const auto recovered_info = read_reply(replica_client, replica_pending);
    assert(recovered_info.find("goblin_ready:1\r\n") != std::string::npos);
    assert(recovered_info.find("replica_state:live\r\n") != std::string::npos);
    assert(recovered_info.find("replica_successful_reconnects:1\r\n") !=
           std::string::npos);
    ::close(restarted_primary_client);

    ::close(replica_client);
  }

  (void)::unlink(socket.c_str());
  (void)::unlink(replica_socket.c_str());
  (void)::unlink(snapshot.c_str());
  (void)::unlink(recovery_snapshot.c_str());
  (void)::unlink(live_snapshot.c_str());
  (void)::unlink(ahead_snapshot.c_str());
  std::cout << "Kafka broker acknowledgements, journal recovery, lineage "
               "filtering, runtime reconnect, live handoff, and RESP2 "
               "ingestion OK\n";
}
