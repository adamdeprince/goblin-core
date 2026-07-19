#include "goblin/core/replication.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <sodium.h>
#include <stdexcept>
#include <utility>

namespace goblin::core {
namespace {

[[nodiscard]] bool response_is_error(std::string_view response) noexcept {
  return response.empty() || response.front() == '-';
}

[[nodiscard]] bool response_is_zero(std::string_view response) noexcept {
  return response == ":0\r\n" || response == "$-1\r\n" ||
         response == "_\r\n";
}

[[nodiscard]] std::string key_for(std::initializer_list<std::string_view> parts) {
  return make_replication_compaction_key(
      std::span<const std::string_view>(parts.begin(), parts.size()));
}

[[nodiscard]] ReplicationMutation command_mutation(
    std::initializer_list<std::string_view> fields, std::string kafka_key = {}) {
  return {.kafka_key = std::move(kafka_key),
          .payload = encode_resp2_command(
              std::span<const std::string_view>(fields.begin(), fields.size()))};
}

void append_current_ttl(const Store& store, std::string_view key,
                        std::vector<ReplicationMutation>& out) {
  const auto expiry = store.expiretime_ms(key);
  const std::string ttl_key = key_for({"TTL", key});
  if (expiry >= 0) {
    const std::string when = std::to_string(expiry);
    out.push_back(command_mutation({"PEXPIREAT", key, when}, ttl_key));
  } else {
    out.push_back(command_mutation({"PERSIST", key}, ttl_key));
  }
}

void append_current_string(const Store& store, std::string_view key,
                           std::vector<ReplicationMutation>& out) {
  const std::string compact_key = key_for({"SET", key});
  const auto value = store.get(key);
  if (!value) {
    out.push_back(command_mutation({"DEL", key}, compact_key));
    return;
  }
  const std::string materialized = value->to_string();
  out.push_back(command_mutation({"SET", key, materialized}, compact_key));
  append_current_ttl(store, key, out);
}

void append_current_hash_field(const Store& store, std::string_view key,
                               std::string_view field,
                               std::vector<ReplicationMutation>& out) {
  const std::string compact_key = key_for({"HSET", key, field});
  const auto value = store.hget(key, field);
  if (value) {
    const std::string materialized = value->to_string();
    out.push_back(
        command_mutation({"HSET", key, field, materialized}, compact_key));
  } else {
    out.push_back(command_mutation({"HDEL", key, field}, compact_key));
  }
}

void append_current_zset_member(const Store& store, std::string_view key,
                                std::string_view member,
                                std::vector<ReplicationMutation>& out) {
  const std::string compact_key = key_for({"ZADD", key, member});
  const auto score = store.zscore(key, member);
  if (score) {
    const std::string text = format_score(*score);
    out.push_back(command_mutation({"ZADD", key, text, member}, compact_key));
  } else {
    out.push_back(command_mutation({"ZREM", key, member}, compact_key));
  }
}

void append_current_set_member(const Store& store, std::string_view key,
                               std::string_view member,
                               std::vector<ReplicationMutation>& out) {
  const std::string compact_key = key_for({"SADD", key, member});
  out.push_back(store.sismember(key, member)
                    ? command_mutation({"SADD", key, member}, compact_key)
                    : command_mutation({"SREM", key, member}, compact_key));
}

[[nodiscard]] std::vector<std::string_view> command_fields(
    const Command& command) {
  std::vector<std::string_view> fields;
  fields.reserve(command.args.size() + 1);
  fields.push_back(command.name);
  fields.insert(fields.end(), command.args.begin(), command.args.end());
  return fields;
}

}  // namespace

bool ReplicationId::empty() const noexcept {
  for (const auto byte : bytes) {
    if (byte != 0) return false;
  }
  return true;
}

std::string ReplicationId::hex() const {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    result[i * 2] = digits[bytes[i] >> 4];
    result[i * 2 + 1] = digits[bytes[i] & 0x0f];
  }
  return result;
}

ReplicationId make_replication_id() {
  static const int sodium_ready = sodium_init();
  if (sodium_ready < 0) {
    throw std::runtime_error("libsodium initialization failed");
  }
  ReplicationId id;
  randombytes_buf(id.bytes.data(), id.bytes.size());
  return id;
}

std::optional<ReplicationId> parse_replication_id(
    std::string_view text) noexcept {
  if (text.size() != 32) return std::nullopt;
  const auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  ReplicationId id;
  for (std::size_t i = 0; i < id.bytes.size(); ++i) {
    const int high = nibble(text[i * 2]);
    const int low = nibble(text[i * 2 + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    id.bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return id;
}

std::string encode_resp2_command(std::span<const std::string_view> fields) {
  std::string result;
  resp::append_array_header(result, fields.size());
  for (const auto field : fields) {
    resp::append_bulk_string(result, field);
  }
  return result;
}

std::string make_replication_compaction_key(
    std::span<const std::string_view> components) {
  std::size_t bytes = 1;
  for (const auto component : components) {
    if (component.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("replication compaction component is too large");
    }
    bytes += sizeof(std::uint32_t) + component.size();
  }
  std::string result;
  result.reserve(bytes);
  result.push_back(static_cast<char>(components.size()));
  for (const auto component : components) {
    std::uint32_t length = static_cast<std::uint32_t>(component.size());
    for (unsigned shift = 0; shift < 32; shift += 8) {
      result.push_back(static_cast<char>((length >> shift) & 0xff));
    }
    result.append(component);
  }
  return result;
}

std::vector<ReplicationMutation> build_replication_mutations(
    const Store& store, const Command& command, std::string_view response) {
  std::vector<ReplicationMutation> result;
  if (response_is_error(response)) return result;

  const auto type = command.type;
  switch (type) {
    case CommandType::hset:
    case CommandType::hmset: {
      if (command.args.empty()) return result;
      for (std::size_t i = 1; i + 1 < command.args.size(); i += 2) {
        append_current_hash_field(store, command.args[0], command.args[i], result);
      }
      return result;
    }
    case CommandType::hsetnx:
      if (!response_is_zero(response) && command.args.size() >= 2) {
        append_current_hash_field(store, command.args[0], command.args[1], result);
      }
      return result;
    case CommandType::hincrby:
    case CommandType::hincrbyfloat:
    case CommandType::goblin_hcad:
    case CommandType::goblin_hsetgt:
      if (command.args.size() >= 2) {
        append_current_hash_field(store, command.args[0], command.args[1], result);
      }
      return result;
    case CommandType::hdel:
      if (!command.args.empty()) {
        for (const auto field : command.args.subspan(1)) {
          append_current_hash_field(store, command.args[0], field, result);
        }
      }
      return result;

    case CommandType::zadd: {
      if (command.args.empty()) return result;
      std::size_t first_pair = 1;
      while (first_pair < command.args.size()) {
        const auto option = command.args[first_pair];
        const auto ci_equal = [](std::string_view a, std::string_view b) {
          if (a.size() != b.size()) return false;
          for (std::size_t i = 0; i < a.size(); ++i) {
            const char x = a[i] >= 'a' && a[i] <= 'z' ? a[i] - 32 : a[i];
            if (x != b[i]) return false;
          }
          return true;
        };
        if (ci_equal(option, "NX") || ci_equal(option, "XX") ||
            ci_equal(option, "GT") || ci_equal(option, "LT") ||
            ci_equal(option, "CH") || ci_equal(option, "INCR")) {
          ++first_pair;
        } else {
          break;
        }
      }
      for (std::size_t i = first_pair; i + 1 < command.args.size(); i += 2) {
        append_current_zset_member(store, command.args[0], command.args[i + 1],
                                   result);
      }
      return result;
    }
    case CommandType::zincrby:
      if (command.args.size() >= 3) {
        append_current_zset_member(store, command.args[0], command.args[2], result);
      }
      return result;
    case CommandType::zrem:
      if (!command.args.empty()) {
        for (const auto member : command.args.subspan(1)) {
          append_current_zset_member(store, command.args[0], member, result);
        }
      }
      return result;

    case CommandType::sadd:
    case CommandType::srem:
      if (!command.args.empty()) {
        for (const auto member : command.args.subspan(1)) {
          append_current_set_member(store, command.args[0], member, result);
        }
      }
      return result;

    case CommandType::set:
    case CommandType::getset:
    case CommandType::setnx:
    case CommandType::getdel:
    case CommandType::append:
    case CommandType::incr:
    case CommandType::decr:
    case CommandType::incrby:
    case CommandType::decrby:
    case CommandType::incrbyfloat:
    case CommandType::setrange:
    case CommandType::getex:
    case CommandType::goblin_cad:
    case CommandType::goblin_caexpire:
    case CommandType::goblin_cas:
    case CommandType::goblin_increx:
    case CommandType::goblin_incrbound:
    case CommandType::goblin_decrpos:
      if (!command.args.empty() &&
          !(type == CommandType::setnx && response_is_zero(response))) {
        append_current_string(store, command.args[0], result);
      }
      return result;

    case CommandType::mset:
    case CommandType::msetnx:
      if (type == CommandType::msetnx && response_is_zero(response)) return result;
      for (std::size_t i = 0; i + 1 < command.args.size(); i += 2) {
        append_current_string(store, command.args[i], result);
      }
      return result;

    case CommandType::del:
      for (const auto key : command.args) {
        result.push_back(command_mutation({"DEL", key}, key_for({"SET", key})));
      }
      return result;

    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat:
    case CommandType::persist:
      if (!command.args.empty()) append_current_ttl(store, command.args[0], result);
      return result;

    default:
      break;
  }

  // Ordered operations that cannot be compacted safely retain their canonical
  // client command. This includes lists, ranges, moves, and multi-key stores.
  auto fields = command_fields(command);
  result.push_back(
      {.kafka_key = {}, .payload = encode_resp2_command(fields)});
  return result;
}

std::string encode_firehose_batch(const ReplicationBatch& batch) {
  const std::string id = batch.id.hex();
  const std::string offset = std::to_string(batch.offset);
  const std::string version = std::to_string(kReplicationProtocolVersion);
  std::string result;
  resp::append_array_header(result, 4 + batch.mutations.size());
  resp::append_bulk_string(result, "GOBLIN.REPL");
  resp::append_bulk_string(result, version);
  resp::append_bulk_string(result, id);
  resp::append_bulk_string(result, offset);
  for (const auto& mutation : batch.mutations) {
    resp::append_bulk_string(result, mutation.payload);
  }
  return result;
}

std::string encode_firehose_hello(const ReplicationState& state) {
  const std::string version = std::to_string(kReplicationProtocolVersion);
  const std::string id = state.id.hex();
  const std::string offset = std::to_string(state.offset);
  const std::array<std::string_view, 4> fields{
      "GOBLIN.FIREHOSE", version, id, offset};
  return resp::array(fields);
}

}  // namespace goblin::core
