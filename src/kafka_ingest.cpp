#include "goblin/core/kafka_ingest.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/store.hpp"

#include <rdkafka.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace goblin::core {
namespace {

constexpr int kBrokerTimeoutMs = 15'000;
constexpr auto kCatchUpStallTimeout = std::chrono::seconds(30);

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

[[nodiscard]] std::optional<std::string> percent_decode(
    std::string_view value, std::string& error) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%') {
      decoded.push_back(value[i]);
      continue;
    }
    if (i + 2 >= value.size()) {
      error = "incomplete percent escape in Kafka connection string";
      return std::nullopt;
    }
    const int high = hex_value(value[i + 1]);
    const int low = hex_value(value[i + 2]);
    if (high < 0 || low < 0) {
      error = "invalid percent escape in Kafka connection string";
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return decoded;
}

[[nodiscard]] bool is_reserved_property(std::string_view name) noexcept {
  return name == "bootstrap.servers" || name == "brokers" || name == "topic" ||
         name == "group.id" || name == "enable.auto.commit" ||
         name == "enable.auto.offset.store" ||
         name == "enable.partition.eof" || name == "auto.offset.reset" ||
         name == "allow.auto.create.topics";
}

[[nodiscard]] bool add_property(KafkaConnectionOptions& options,
                                std::string name, std::string value,
                                std::string& error) {
  if (name.empty() || value.empty()) {
    error = "Kafka connection properties require non-empty names and values";
    return false;
  }
  if (is_reserved_property(name)) {
    error = "Kafka connection property '" + name + "' is managed by goblin-core";
    return false;
  }
  const auto duplicate =
      std::find_if(options.properties.begin(), options.properties.end(),
                   [&name](const auto& property) { return property.first == name; });
  if (duplicate != options.properties.end()) {
    error = "duplicate Kafka connection property '" + name + "'";
    return false;
  }
  options.properties.emplace_back(std::move(name), std::move(value));
  return true;
}

[[nodiscard]] bool parse_query(KafkaConnectionOptions& options,
                               std::string_view query, std::string& error) {
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto item = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{}
                                          : query.substr(amp + 1);
    if (item.empty()) continue;
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      error = "Kafka URI query properties must use name=value";
      return false;
    }
    auto name = percent_decode(item.substr(0, equals), error);
    auto value = percent_decode(item.substr(equals + 1), error);
    if (!name || !value || !add_property(options, std::move(*name),
                                          std::move(*value), error)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool set_conf(rd_kafka_conf_t* conf, std::string_view name,
                            std::string_view value, std::string& error) {
  std::array<char, 512> buffer{};
  const std::string name_string(name);
  const std::string value_string(value);
  if (rd_kafka_conf_set(conf, name_string.c_str(), value_string.c_str(),
                        buffer.data(), buffer.size()) != RD_KAFKA_CONF_OK) {
    error = "Kafka option '" + name_string + "': " + buffer.data();
    return false;
  }
  return true;
}

[[nodiscard]] bool set_nonblocking_cloexec(int fd) noexcept {
  const int status = ::fcntl(fd, F_GETFL, 0);
  const int descriptor = ::fcntl(fd, F_GETFD, 0);
  return status >= 0 && descriptor >= 0 &&
         ::fcntl(fd, F_SETFL, status | O_NONBLOCK) == 0 &&
         ::fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == 0;
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

[[nodiscard]] bool transient_kafka_error(rd_kafka_resp_err_t error) noexcept {
  switch (error) {
    case RD_KAFKA_RESP_ERR__TIMED_OUT:
    case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
    case RD_KAFKA_RESP_ERR__TRANSPORT:
    case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
    case RD_KAFKA_RESP_ERR__WAIT_COORD:
    case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
    case RD_KAFKA_RESP_ERR_NOT_LEADER_OR_FOLLOWER:
    case RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE:
    case RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::string resp_error_text(std::string_view response) {
  if (!response.empty() && response.front() == '-') response.remove_prefix(1);
  if (const auto crlf = response.find("\r\n"); crlf != std::string_view::npos) {
    response = response.substr(0, crlf);
  }
  return std::string(response);
}

}  // namespace

std::optional<KafkaConnectionOptions> parse_kafka_connection_string(
    std::string_view connection, std::string& error) {
  error.clear();
  KafkaConnectionOptions options;

  constexpr std::string_view scheme = "kafka://";
  if (connection.starts_with(scheme)) {
    connection.remove_prefix(scheme.size());
    const auto query_at = connection.find('?');
    const auto address_and_topic = connection.substr(0, query_at);
    const auto query = query_at == std::string_view::npos
                           ? std::string_view{}
                           : connection.substr(query_at + 1);
    const auto slash = address_and_topic.find('/');
    if (slash == std::string_view::npos) {
      error = "Kafka URI must be kafka://BROKERS/TOPIC";
      return std::nullopt;
    }
    options.brokers = std::string(trim(address_and_topic.substr(0, slash)));
    auto topic = percent_decode(address_and_topic.substr(slash + 1), error);
    if (!topic) return std::nullopt;
    options.topic = std::move(*topic);
    if (options.brokers.empty() || options.topic.empty()) {
      error = "Kafka URI requires both bootstrap brokers and a topic";
      return std::nullopt;
    }
    if (!parse_query(options, query, error)) return std::nullopt;
    return options;
  }

  while (!connection.empty()) {
    const auto semicolon = connection.find(';');
    auto item = trim(connection.substr(0, semicolon));
    connection = semicolon == std::string_view::npos
                     ? std::string_view{}
                     : connection.substr(semicolon + 1);
    if (item.empty()) continue;
    const auto equals = item.find('=');
    if (equals == std::string_view::npos) {
      error = "Kafka connection properties must use name=value";
      return std::nullopt;
    }
    std::string name(trim(item.substr(0, equals)));
    std::string value(trim(item.substr(equals + 1)));
    if (name == "bootstrap.servers" || name == "brokers") {
      if (!options.brokers.empty()) {
        error = "duplicate Kafka bootstrap broker property";
        return std::nullopt;
      }
      options.brokers = std::move(value);
    } else if (name == "topic") {
      if (!options.topic.empty()) {
        error = "duplicate Kafka topic property";
        return std::nullopt;
      }
      options.topic = std::move(value);
    } else if (!add_property(options, std::move(name), std::move(value), error)) {
      return std::nullopt;
    }
  }

  if (options.brokers.empty() || options.topic.empty()) {
    error = "Kafka connection string requires bootstrap.servers and topic";
    return std::nullopt;
  }
  return options;
}

KafkaRecordResult apply_kafka_resp2_record(Store& store,
                                           std::string_view payload,
                                           std::string& error) {
  error.clear();
  if (payload.empty() || payload.front() != '*') {
    error = "Kafka record is not a RESP2 array";
    return KafkaRecordResult::error;
  }

  RespParser parser;
  parser.append(payload);
  if (parser.has_error()) {
    error = parser.error();
    return KafkaRecordResult::error;
  }
  if (parser.has_unparsed_input()) {
    error = "Kafka record contains an incomplete RESP2 command";
    return KafkaRecordResult::error;
  }
  auto frame = parser.pop();
  if (!frame || parser.has_queued_frames()) {
    error = "Kafka record must contain exactly one RESP2 command";
    return KafkaRecordResult::error;
  }

  auto parsed = parse_command(frame->fields);
  if (!parsed.ok()) {
    error = parsed.error;
    return KafkaRecordResult::error;
  }
  if (parsed.command->type == CommandType::unknown) {
    error = "unknown Kafka command '" + std::string(parsed.command->name) + "'";
    return KafkaRecordResult::error;
  }
  if (!command_mutates_store(parsed.command->type)) {
    return KafkaRecordResult::filtered;
  }

  std::string response;
  execute_command_into(store, *parsed.command, response);
  if (!response.empty() && response.front() == '-') {
    error = "Kafka write failed: " + resp_error_text(response);
    return KafkaRecordResult::error;
  }
  return KafkaRecordResult::applied;
}

struct KafkaIngestor::Impl {
  struct PartitionProgress {
    std::int32_t partition{0};
    std::int64_t next_offset{0};
    std::int64_t startup_high_watermark{0};
  };

  rd_kafka_t* consumer{nullptr};
  rd_kafka_queue_t* consumer_queue{nullptr};
  int notify_read_fd{-1};
  int notify_write_fd{-1};
  std::string topic;
  std::string description;
  std::vector<PartitionProgress> partitions;

  ~Impl() {
    if (consumer_queue != nullptr) {
      rd_kafka_queue_io_event_enable(consumer_queue, -1, nullptr, 0);
      rd_kafka_queue_destroy(consumer_queue);
      consumer_queue = nullptr;
    }
    close_fd(notify_read_fd);
    close_fd(notify_write_fd);
    if (consumer != nullptr) {
      (void)rd_kafka_assign(consumer, nullptr);
      (void)rd_kafka_consumer_close(consumer);
      rd_kafka_destroy(consumer);
      consumer = nullptr;
    }
  }

  void drain_notification() noexcept {
    std::array<char, 256> bytes{};
    while (::read(notify_read_fd, bytes.data(), bytes.size()) > 0) {
    }
  }

  [[nodiscard]] PartitionProgress* progress_for(std::int32_t partition) noexcept {
    const auto found =
        std::find_if(partitions.begin(), partitions.end(),
                     [partition](const auto& value) {
                       return value.partition == partition;
                     });
    return found == partitions.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool startup_complete() const noexcept {
    return std::all_of(partitions.begin(), partitions.end(), [](const auto& value) {
      return value.next_offset >= value.startup_high_watermark;
    });
  }

  [[nodiscard]] bool handle_message(Store& store, rd_kafka_message_t& message,
                                    KafkaReplayStats& stats,
                                    std::string& error) {
    if (message.err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      if (auto* progress = progress_for(message.partition)) {
        progress->next_offset = std::max(progress->next_offset, message.offset);
      }
      return true;
    }
    if (message.err != RD_KAFKA_RESP_ERR_NO_ERROR) {
      if (transient_kafka_error(message.err)) return true;
      error = "Kafka consumer error: " + std::string(rd_kafka_message_errstr(&message));
      return false;
    }

    ++stats.records;
    if (message.payload == nullptr) {
      error = "Kafka " + topic + '[' + std::to_string(message.partition) +
              "] offset " + std::to_string(message.offset) +
              ": tombstone record has no RESP2 command value";
      return false;
    }
    const std::string_view payload(static_cast<const char*>(message.payload),
                                   message.len);
    switch (apply_kafka_resp2_record(store, payload, error)) {
      case KafkaRecordResult::applied:
        ++stats.writes;
        break;
      case KafkaRecordResult::filtered:
        ++stats.filtered;
        break;
      case KafkaRecordResult::error:
        error = "Kafka " + topic + '[' + std::to_string(message.partition) +
                "] offset " + std::to_string(message.offset) + ": " + error;
        return false;
    }
    if (auto* progress = progress_for(message.partition)) {
      progress->next_offset =
          std::max(progress->next_offset, message.offset + 1);
    }
    return true;
  }
};

KafkaIngestor::KafkaIngestor(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

KafkaIngestor::~KafkaIngestor() = default;

std::unique_ptr<KafkaIngestor> KafkaIngestor::connect(
    std::string_view connection,
    std::optional<std::int64_t> start_timestamp_ms,
    std::string& error) {
  auto options = parse_kafka_connection_string(connection, error);
  if (!options) return nullptr;

  rd_kafka_conf_t* conf = rd_kafka_conf_new();
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string group_id =
      "goblin-core-" + std::to_string(static_cast<long long>(::getpid())) + '-' +
      std::to_string(static_cast<long long>(now));
  if (!set_conf(conf, "bootstrap.servers", options->brokers, error) ||
      !set_conf(conf, "group.id", group_id, error) ||
      !set_conf(conf, "enable.auto.commit", "false", error) ||
      !set_conf(conf, "enable.auto.offset.store", "false", error) ||
      !set_conf(conf, "enable.partition.eof", "true", error) ||
      !set_conf(conf, "auto.offset.reset", "earliest", error) ||
      !set_conf(conf, "allow.auto.create.topics", "false", error)) {
    rd_kafka_conf_destroy(conf);
    return nullptr;
  }
  for (const auto& [name, value] : options->properties) {
    if (!set_conf(conf, name, value, error)) {
      rd_kafka_conf_destroy(conf);
      return nullptr;
    }
  }

  std::array<char, 512> kafka_error{};
  rd_kafka_t* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf,
                                      kafka_error.data(), kafka_error.size());
  if (consumer == nullptr) {
    error = "cannot create Kafka consumer: " + std::string(kafka_error.data());
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  impl->consumer = consumer;
  impl->topic = options->topic;
  impl->description = options->brokers + '/' + options->topic;

  if (const auto poll_error = rd_kafka_poll_set_consumer(consumer);
      poll_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
    error = "cannot configure Kafka consumer queue: " +
            std::string(rd_kafka_err2str(poll_error));
    return nullptr;
  }

  rd_kafka_topic_t* topic =
      rd_kafka_topic_new(consumer, options->topic.c_str(), nullptr);
  if (topic == nullptr) {
    error = "cannot create Kafka topic handle: " +
            std::string(rd_kafka_err2str(rd_kafka_last_error()));
    return nullptr;
  }
  const rd_kafka_metadata_t* metadata = nullptr;
  const auto metadata_error =
      rd_kafka_metadata(consumer, 0, topic, &metadata, kBrokerTimeoutMs);
  rd_kafka_topic_destroy(topic);
  if (metadata_error != RD_KAFKA_RESP_ERR_NO_ERROR || metadata == nullptr) {
    error = "cannot read Kafka topic metadata: " +
            std::string(rd_kafka_err2str(metadata_error));
    return nullptr;
  }
  std::unique_ptr<const rd_kafka_metadata_t, decltype(&rd_kafka_metadata_destroy)>
      metadata_guard(metadata, &rd_kafka_metadata_destroy);
  if (metadata->topic_cnt != 1 || metadata->topics[0].err != 0) {
    const auto topic_error = metadata->topic_cnt == 1
                                 ? metadata->topics[0].err
                                 : RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART;
    error = "cannot read Kafka topic '" + options->topic + "': " +
            std::string(rd_kafka_err2str(topic_error));
    return nullptr;
  }

  const auto& topic_metadata = metadata->topics[0];
  rd_kafka_topic_partition_list_t* assignment =
      rd_kafka_topic_partition_list_new(topic_metadata.partition_cnt);
  if (assignment == nullptr) {
    error = "cannot allocate Kafka partition assignment";
    return nullptr;
  }
  std::unique_ptr<rd_kafka_topic_partition_list_t,
                  decltype(&rd_kafka_topic_partition_list_destroy)>
      assignment_guard(assignment, &rd_kafka_topic_partition_list_destroy);

  impl->partitions.reserve(static_cast<std::size_t>(topic_metadata.partition_cnt));
  for (int i = 0; i < topic_metadata.partition_cnt; ++i) {
    const auto partition = topic_metadata.partitions[i].id;
    std::int64_t low = 0;
    std::int64_t high = 0;
    const auto watermark_error = rd_kafka_query_watermark_offsets(
        consumer, options->topic.c_str(), partition, &low, &high,
        kBrokerTimeoutMs);
    if (watermark_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
      error = "cannot read Kafka watermarks for partition " +
              std::to_string(partition) + ": " +
              rd_kafka_err2str(watermark_error);
      return nullptr;
    }
    auto* item = rd_kafka_topic_partition_list_add(
        assignment, options->topic.c_str(), partition);
    item->offset = start_timestamp_ms.value_or(low);
    impl->partitions.push_back(Impl::PartitionProgress{
        .partition = partition,
        .next_offset = low,
        .startup_high_watermark = high});
  }

  if (start_timestamp_ms) {
    const auto offset_error =
        rd_kafka_offsets_for_times(consumer, assignment, kBrokerTimeoutMs);
    if (offset_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
      error = "cannot resolve Kafka timestamp offsets: " +
              std::string(rd_kafka_err2str(offset_error));
      return nullptr;
    }
  }

  for (int i = 0; i < assignment->cnt; ++i) {
    auto& item = assignment->elems[i];
    auto& progress = impl->partitions[static_cast<std::size_t>(i)];
    if (item.err != RD_KAFKA_RESP_ERR_NO_ERROR) {
      error = "cannot resolve Kafka offset for partition " +
              std::to_string(item.partition) + ": " + rd_kafka_err2str(item.err);
      return nullptr;
    }
    if (item.offset < 0) item.offset = progress.startup_high_watermark;
    item.offset = std::min(item.offset, progress.startup_high_watermark);
    progress.next_offset = item.offset;
  }

  if (const auto assign_error = rd_kafka_assign(consumer, assignment);
      assign_error != RD_KAFKA_RESP_ERR_NO_ERROR) {
    error = "cannot assign Kafka partitions: " +
            std::string(rd_kafka_err2str(assign_error));
    return nullptr;
  }

  int pipe_fds[2] = {-1, -1};
  if (::pipe(pipe_fds) != 0 || !set_nonblocking_cloexec(pipe_fds[0]) ||
      !set_nonblocking_cloexec(pipe_fds[1])) {
    if (pipe_fds[0] >= 0) (void)::close(pipe_fds[0]);
    if (pipe_fds[1] >= 0) (void)::close(pipe_fds[1]);
    error = "cannot create Kafka event notification pipe: " +
            std::string(std::strerror(errno));
    return nullptr;
  }
  impl->notify_read_fd = pipe_fds[0];
  impl->notify_write_fd = pipe_fds[1];
  impl->consumer_queue = rd_kafka_queue_get_consumer(consumer);
  if (impl->consumer_queue == nullptr) {
    error = "cannot acquire Kafka consumer queue";
    return nullptr;
  }
  constexpr char notification = 'K';
  rd_kafka_queue_io_event_enable(impl->consumer_queue, impl->notify_write_fd,
                                 &notification, sizeof(notification));

  return std::unique_ptr<KafkaIngestor>(
      new KafkaIngestor(std::move(impl)));
}

bool KafkaIngestor::catch_up(Store& store, KafkaReplayStats& stats,
                             std::string& error) {
  error.clear();
  auto last_progress = std::chrono::steady_clock::now();
  while (!impl_->startup_complete()) {
    impl_->drain_notification();
    std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)>
        message(rd_kafka_consumer_poll(impl_->consumer, 100),
                &rd_kafka_message_destroy);
    if (message == nullptr) {
      if (std::chrono::steady_clock::now() - last_progress >
          kCatchUpStallTimeout) {
        error = "Kafka startup replay made no progress for 30 seconds";
        return false;
      }
      continue;
    }
    const auto before = stats.records;
    if (!impl_->handle_message(store, *message, stats, error)) return false;
    if (stats.records != before || message->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
      last_progress = std::chrono::steady_clock::now();
    }
  }
  impl_->drain_notification();
  return true;
}

KafkaPollResult KafkaIngestor::poll(Store& store, std::size_t max_records) {
  KafkaPollResult result;
  impl_->drain_notification();
  if (max_records == 0) return result;

  std::size_t events = 0;
  const std::size_t max_events =
      max_records > (std::numeric_limits<std::size_t>::max() - 16) / 4
          ? std::numeric_limits<std::size_t>::max()
          : max_records * 4 + 16;
  while (result.stats.records < max_records && events++ < max_events) {
    std::unique_ptr<rd_kafka_message_t, decltype(&rd_kafka_message_destroy)>
        message(rd_kafka_consumer_poll(impl_->consumer, 0),
                &rd_kafka_message_destroy);
    if (message == nullptr) break;
    if (!impl_->handle_message(store, *message, result.stats, result.error)) {
      return result;
    }
  }
  result.may_have_more = rd_kafka_queue_length(impl_->consumer_queue) != 0;
  return result;
}

int KafkaIngestor::notification_fd() const noexcept {
  return impl_->notify_read_fd;
}

bool KafkaIngestor::has_pending() const noexcept {
  return rd_kafka_queue_length(impl_->consumer_queue) != 0;
}

std::string_view KafkaIngestor::description() const noexcept {
  return impl_->description;
}

}  // namespace goblin::core
