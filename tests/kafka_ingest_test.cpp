#include "goblin/core/command.hpp"
#include "goblin/core/kafka_ingest.hpp"
#include "goblin/core/store.hpp"

#include <array>
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::string resp2(
    std::initializer_list<std::string_view> fields) {
  std::string result = "*" + std::to_string(fields.size()) + "\r\n";
  for (const auto field : fields) {
    result += "$" + std::to_string(field.size()) + "\r\n";
    result.append(field);
    result += "\r\n";
  }
  return result;
}

}  // namespace

int main() {
  using namespace goblin::core;

  std::string error;
  const auto uri = parse_kafka_connection_string(
      "kafka://one:9092,two:9092/orders%2Fwrite?client.id=edge%201", error);
  assert(uri && error.empty());
  assert(uri->brokers == "one:9092,two:9092");
  assert(uri->topic == "orders/write");
  assert(uri->properties.size() == 1);
  assert(uri->properties[0].first == "client.id");
  assert(uri->properties[0].second == "edge 1");

  const auto properties = parse_kafka_connection_string(
      "bootstrap.servers=localhost:9092;topic=writes;client.id=test", error);
  assert(properties && properties->topic == "writes");
  assert(!parse_kafka_connection_string(
      "kafka://localhost:9092/writes?enable.auto.commit=true", error));
  assert(!error.empty());

  Store store;
  assert(apply_kafka_resp2_record(store, resp2({"SET", "key", "value"}),
                                  error) == KafkaRecordResult::applied);
  assert(store.get("key") == std::optional<std::string_view>{"value"});

  assert(apply_kafka_resp2_record(store, resp2({"GET", "key"}), error) ==
         KafkaRecordResult::filtered);
  assert(apply_kafka_resp2_record(store, resp2({"PING"}), error) ==
         KafkaRecordResult::filtered);
  assert(apply_kafka_resp2_record(
             store, resp2({"EVAL", "return redis.call('SET','x','y')", "0"}),
             error) == KafkaRecordResult::filtered);
  assert(!store.get("x"));

  assert(apply_kafka_resp2_record(store, "SET key value\r\n", error) ==
         KafkaRecordResult::error);
  assert(apply_kafka_resp2_record(store, "*2\r\n$3\r\nGET\r\n", error) ==
         KafkaRecordResult::error);
  const auto two = resp2({"SET", "a", "1"}) + resp2({"SET", "b", "2"});
  assert(apply_kafka_resp2_record(store, two, error) ==
         KafkaRecordResult::error);
  assert(!store.get("a") && !store.get("b"));
  assert(apply_kafka_resp2_record(store, resp2({"NOT.A.COMMAND"}), error) ==
         KafkaRecordResult::error);
  assert(apply_kafka_resp2_record(store, resp2({"SET", "only-a-key"}), error) ==
         KafkaRecordResult::error);

  assert(command_mutates_store(CommandType::hset));
  assert(command_mutates_store(CommandType::segmented_lpush));
  assert(!command_mutates_store(CommandType::hget));
  assert(!command_mutates_store(CommandType::goblin_load));
  assert(!command_mutates_store(CommandType::eval));

  const auto lineage = make_replication_id();
  const auto old_lineage = make_replication_id();
  Store recovering;
  recovering.set_replication_state(ReplicationState{
      .id = lineage,
      .offset = 41,
      .kafka_acknowledged_offset = 9001,
      .valid = true});

  // Old lineages are rejected before RESP parsing. This is what makes it safe
  // to seek a conservative Kafka offset that starts a little before the
  // snapshot's lineage.
  assert(apply_kafka_replication_record(recovering, old_lineage, 400,
                                         "not RESP", error) ==
         KafkaRecordResult::filtered);
  assert(error.empty());
  assert(recovering.replication_state().offset == 41);

  // The inclusive boundary and any later duplicate are already in the snapshot.
  assert(apply_kafka_replication_record(
             recovering, lineage, 41, resp2({"INCR", "counter"}), error) ==
         KafkaRecordResult::filtered);
  assert(!recovering.get("counter"));

  // Compaction may leave offset holes. A later record from the same lineage is
  // valid and advances the logical cursor without requiring adjacency.
  assert(apply_kafka_replication_record(
             recovering, lineage, 44, resp2({"SET", "counter", "44"}), error) ==
         KafkaRecordResult::applied);
  assert(recovering.get("counter") == std::optional<std::string_view>{"44"});
  assert(recovering.replication_state().offset == 44);
  assert(recovering.replication_state().kafka_acknowledged_offset == 9001);

  assert(apply_kafka_replication_record(
             recovering, lineage, 45, resp2({"GET", "counter"}), error) ==
         KafkaRecordResult::error);
  assert(recovering.replication_state().offset == 44);

  std::stringstream snapshot;
  recovering.save(snapshot, false);
  Store restored;
  (void)restored.load(snapshot);
  assert(restored.replication_state().valid);
  assert(restored.replication_state().id == lineage);
  assert(restored.replication_state().offset == 44);
  assert(restored.replication_state().kafka_acknowledged_offset == 9001);

  // A failed load must not expose replication metadata parsed from a partial
  // snapshot, even though the ordinary keyspace is cleared on load failure.
  auto truncated = snapshot.str();
  truncated.pop_back();
  Store failed_restore;
  const auto original_state = failed_restore.replication_state();
  std::stringstream broken(truncated);
  bool threw = false;
  try {
    (void)failed_restore.load(broken);
  } catch (const std::exception&) {
    threw = true;
  }
  assert(threw);
  assert(failed_restore.replication_state().id == original_state.id);
  assert(failed_restore.replication_state().offset == original_state.offset);
  assert(failed_restore.replication_state().kafka_acknowledged_offset ==
         original_state.kafka_acknowledged_offset);

  Store canonical;
  const std::array<std::string_view, 4> first_zadd{
      "ZADD", "leaders", "1", "alice"};
  auto parsed_zadd = parse_command(first_zadd);
  assert(parsed_zadd.ok());
  std::string zadd_reply;
  execute_command_into(canonical, *parsed_zadd.command, zadd_reply);
  const auto first_mutations = build_replication_mutations(
      canonical, *parsed_zadd.command, zadd_reply);
  assert(first_mutations.size() == 1);
  const std::array<std::string_view, 3> zadd_identity{
      "ZADD", "leaders", "alice"};
  assert(first_mutations[0].kafka_key ==
         make_replication_compaction_key(zadd_identity));

  const std::array<std::string_view, 4> second_zadd{
      "ZADD", "leaders", "2", "alice"};
  parsed_zadd = parse_command(second_zadd);
  assert(parsed_zadd.ok());
  zadd_reply.clear();
  execute_command_into(canonical, *parsed_zadd.command, zadd_reply);
  const auto second_mutations = build_replication_mutations(
      canonical, *parsed_zadd.command, zadd_reply);
  assert(second_mutations.size() == 1);
  assert(second_mutations[0].kafka_key == first_mutations[0].kafka_key);
  assert(second_mutations[0].payload ==
         resp2({"ZADD", "leaders", "2", "alice"}));

  std::cout << "Kafka RESP2 filtering and replication recovery state OK\n";
}
