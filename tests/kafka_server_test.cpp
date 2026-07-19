#include "goblin/core/kafka_ingest.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/store.hpp"
#include "socket_test_utils.hpp"

#include <rdkafka.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

struct Child {
  pid_t pid{-1};
  ~Child() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
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
  (void)::unlink(socket.c_str());
  (void)::unlink(snapshot.c_str());
  (void)::unlink(recovery_snapshot.c_str());
  Producer producer(argv[2], argv[3]);
  producer.send({"SET", "startup-key", "from-backlog"}, 0);
  producer.send({"GET", "startup-key"}, 0);
  producer.send({"HSET", "startup-hash", "field", "value"}, 0);

  const std::string connection =
      "kafka://" + std::string(argv[2]) + '/' + argv[3];
  {
    const auto tcp_port = goblin::test::reserve_loopback_tcp_port();
    assert(tcp_port != 0);
    auto server = spawn_server(
        argv[1], {"--uds-listen", socket, "--port", std::to_string(tcp_port),
                  "--kafka", connection});
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

    send_command(client, {"SET", "journaled-key", "from-client"});
    assert(read_reply(client, pending) == "+OK\r\n");

    // Force at least one later event-loop pass so the producer delivery callback
    // has published its broker offset into the Store before the forked save.
    bool acknowledged = false;
    for (int attempt = 0; attempt < 500 && !acknowledged; ++attempt) {
      send_command(client, {"INFO"});
      const auto info = read_reply(client, pending);
      acknowledged =
          info.find("kafka_acknowledged_offset:-1\r\n") == std::string::npos;
      if (!acknowledged) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(acknowledged);

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

  (void)::unlink(socket.c_str());
  (void)::unlink(snapshot.c_str());
  (void)::unlink(recovery_snapshot.c_str());
  std::cout << "Kafka journal, lineage filtering, inclusive snapshot recovery, "
               "and live RESP2 ingestion OK\n";
}
