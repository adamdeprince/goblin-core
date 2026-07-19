#include "goblin/core/command.hpp"
#include "goblin/core/kafka_ingest.hpp"
#include "goblin/core/store.hpp"

#include <cassert>
#include <initializer_list>
#include <iostream>
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

  std::cout << "Kafka RESP2 write filtering OK\n";
}
