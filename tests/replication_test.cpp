#include "goblin/core/command.hpp"
#include "goblin/core/replication.hpp"
#include "goblin/core/store.hpp"

#undef NDEBUG
#include <array>
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using goblin::core::Command;
using goblin::core::Store;

[[nodiscard]] std::string execute(
    Store& store, std::initializer_list<std::string_view> fields,
    Command* parsed_command = nullptr) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  if (parsed_command != nullptr) *parsed_command = *parsed.command;
  return goblin::core::execute_command(store, *parsed.command);
}

[[nodiscard]] goblin::core::ReplicationMutation mutation(
    std::initializer_list<std::string_view> fields) {
  return {.payload = goblin::core::encode_resp2_command(
              std::span<const std::string_view>(fields.begin(), fields.size()))};
}

}  // namespace

int main() {
  using namespace goblin::core;

  const auto id = make_replication_id();
  const FirehoseHello hello{.id = id, .offset = 17};
  std::string error;
  const auto decoded_hello = decode_firehose_hello(encode_firehose_hello(
                                                       ReplicationState{
                                                           .id = id,
                                                           .offset = 17,
                                                           .valid = true}),
                                                   error);
  assert(decoded_hello && decoded_hello->id == hello.id &&
         decoded_hello->offset == hello.offset);

  ReplicationBatch batch{
      .id = id,
      .offset = 1,
      .mutations = {mutation({"SET", "alpha", "1"}),
                    mutation({"HSET", "hash", "field", "value"})}};
  const auto decoded = decode_firehose_batch(encode_firehose_batch(batch), error);
  assert(decoded && decoded->id == id && decoded->offset == 1);
  assert(decoded->mutations.size() == 2);

  Store replica;
  replica.set_replication_state(
      ReplicationState{.id = id, .offset = 0, .valid = true});
  assert(apply_firehose_batch(replica, *decoded, error));
  assert(replica.replication_state().offset == 2);
  assert(replica.get("alpha") == std::optional<std::string_view>{"1"});
  assert(replica.hget("hash", "field")->to_string() == "value");

  // A repeated batch and a partially overlapping batch are both idempotent.
  assert(apply_firehose_batch(replica, *decoded, error));
  ReplicationBatch overlap{
      .id = id,
      .offset = 2,
      .mutations = {mutation({"HSET", "hash", "field", "ignored"}),
                    mutation({"SET", "beta", "2"})}};
  assert(apply_firehose_batch(replica, overlap, error));
  assert(replica.replication_state().offset == 3);
  assert(replica.hget("hash", "field")->to_string() == "value");
  assert(replica.get("beta") == std::optional<std::string_view>{"2"});

  ReplicationBatch gap{.id = id,
                       .offset = 5,
                       .mutations = {mutation({"SET", "gap", "bad"})}};
  assert(!apply_firehose_batch(replica, gap, error));
  assert(error.find("gap") != std::string::npos);
  assert(!replica.get("gap"));

  ReplicationBatch foreign{
      .id = make_replication_id(),
      .offset = 4,
      .mutations = {mutation({"SET", "foreign", "bad"})}};
  assert(!apply_firehose_batch(replica, foreign, error));
  assert(!replica.get("foreign"));

  // SPOP is random on the source. Its replication form must name the exact
  // selected members rather than asking the replica to make another choice.
  Store source;
  Store target;
  for (auto* store : std::array{&source, &target}) {
    assert(execute(*store, {"SADD", "set", "a", "b", "c", "d"}) ==
           ":4\r\n");
  }
  std::vector<std::string_view> spop_fields{"SPOP", "set", "2"};
  auto parsed_spop = parse_command(spop_fields);
  assert(parsed_spop.ok());
  const auto spop_reply = execute_command(source, *parsed_spop.command);
  const auto spop_mutations =
      build_replication_mutations(source, *parsed_spop.command, spop_reply);
  assert(spop_mutations.size() == 2);
  target.set_replication_state(
      ReplicationState{.id = id, .offset = 0, .valid = true});
  const ReplicationBatch spop_batch{
      .id = id, .offset = 1, .mutations = spop_mutations};
  assert(apply_firehose_batch(target, spop_batch, error));
  assert(source.scard("set") == target.scard("set"));
  for (const std::string_view member : {"a", "b", "c", "d"}) {
    assert(source.sismember("set", member) == target.sismember("set", member));
  }

  assert(!command_mutates_store(CommandType::goblin_td_leaderboard_rescore));
  std::cout << "Replication lineage, overlap, and deterministic replay OK\n";
  return 0;
}
