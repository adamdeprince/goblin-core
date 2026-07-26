#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using goblin::core::PubSubKind;
using goblin::core::SbeRingClient;

enum class Mode { wildcard, literal };

struct Config {
  std::string ring;
  std::string channels_file;
  std::string completion_channel;
  std::string ready_file;
  std::uint64_t progress_every{10'000'000};
  Mode mode{Mode::wildcard};
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

[[nodiscard]] std::string_view next_arg(int argc, char** argv, int& index,
                                        std::string_view option) {
  if (++index >= argc) {
    fail(std::string(option) + " requires a value");
  }
  return argv[index];
}

[[nodiscard]] std::uint64_t parse_u64(std::string_view text,
                                      std::string_view context) {
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    fail("invalid integer for " + std::string(context) + ": " +
         std::string(text));
  }
  return value;
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
  Config config;
  bool mode_selected = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--ring") {
      config.ring = next_arg(argc, argv, index, option);
    } else if (option == "--pattern") {
      const auto pattern = next_arg(argc, argv, index, option);
      if (pattern != "*") {
        fail("this benchmark's wildcard mode requires --pattern '*'");
      }
      config.mode = Mode::wildcard;
      mode_selected = true;
    } else if (option == "--channels") {
      config.channels_file = next_arg(argc, argv, index, option);
      config.mode = Mode::literal;
      mode_selected = true;
    } else if (option == "--completion-channel") {
      config.completion_channel = next_arg(argc, argv, index, option);
    } else if (option == "--ready-file") {
      config.ready_file = next_arg(argc, argv, index, option);
    } else if (option == "--progress-every") {
      config.progress_every =
          parse_u64(next_arg(argc, argv, index, option), option);
    } else if (option == "--help") {
      std::cout
          << "Usage: " << argv[0]
          << " --ring PATH (--pattern '*' | --channels FILE)\n"
             "       --completion-channel CHANNEL --ready-file FILE\n"
             "       [--progress-every N]\n";
      std::exit(0);
    } else {
      fail("unknown option: " + std::string(option));
    }
  }
  if (config.ring.empty() || config.completion_channel.empty() ||
      config.ready_file.empty() || !mode_selected) {
    fail("--ring, one subscription mode, --completion-channel, and "
         "--ready-file are required");
  }
  return config;
}

[[nodiscard]] double seconds_between(Clock::time_point begin,
                                     Clock::time_point end) {
  return std::chrono::duration<double>(end - begin).count();
}

[[nodiscard]] std::vector<std::string> read_channels(
    const std::string& filename, const std::string& completion_channel) {
  std::ifstream input(filename);
  if (!input) {
    fail("could not open channels file " + filename);
  }
  std::vector<std::string> channels;
  std::string channel;
  while (std::getline(input, channel)) {
    if (!channel.empty() && channel.back() == '\r') {
      channel.pop_back();
    }
    if (channel.empty()) {
      fail("empty channel in " + filename);
    }
    channels.push_back(channel);
  }
  if (!input.eof()) {
    fail("failed while reading channels file " + filename);
  }
  if (channels.empty()) {
    fail("channels file is empty: " + filename);
  }
  if (!std::is_sorted(channels.begin(), channels.end()) ||
      std::adjacent_find(channels.begin(), channels.end()) != channels.end()) {
    fail("channels file must be sorted and unique");
  }
  if (std::binary_search(channels.begin(), channels.end(),
                         completion_channel)) {
    fail("completion channel collides with a dataset channel");
  }
  channels.push_back(completion_channel);
  return channels;
}

void subscribe_literals(SbeRingClient& client,
                        const std::vector<std::string>& channels) {
  constexpr std::size_t kMaxNamesPerBatch = 4096;
  constexpr std::size_t kMaxNameBytesPerBatch = 512 * 1024;
  std::vector<std::string_view> views;
  views.reserve(kMaxNamesPerBatch);
  std::size_t begin = 0;
  while (begin < channels.size()) {
    views.clear();
    std::size_t bytes = 0;
    std::size_t end = begin;
    while (end < channels.size() && views.size() < kMaxNamesPerBatch) {
      const std::size_t next_bytes = bytes + channels[end].size();
      if (!views.empty() && next_bytes > kMaxNameBytesPerBatch) {
        break;
      }
      bytes = next_bytes;
      views.push_back(channels[end]);
      ++end;
    }
    const auto acknowledgements =
        client.subscribe(views, std::chrono::minutes(5));
    if (acknowledgements.size() != views.size()) {
      fail("SUBSCRIBE returned the wrong acknowledgement count");
    }
    begin = end;
  }
}

void write_ready_file(const std::string& filename) {
  std::ofstream output(filename, std::ios::trunc);
  if (!output) {
    fail("could not create ready file " + filename);
  }
  output << "ready\n";
  output.close();
  if (!output) {
    fail("could not write ready file " + filename);
  }
}

void run(const Config& config) {
  auto client =
      SbeRingClient::open(config.ring.c_str(), std::chrono::seconds(30));
  if (!client) {
    fail("could not open SBE ring " + config.ring);
  }

  std::vector<std::string> literal_channels;
  const auto subscribe_started = Clock::now();
  if (config.mode == Mode::wildcard) {
    const std::string_view pattern("*");
    const auto acknowledgements =
        client->psubscribe(std::span(&pattern, 1), std::chrono::minutes(5));
    if (acknowledgements.size() != 1) {
      fail("PSUBSCRIBE returned the wrong acknowledgement count");
    }
  } else {
    literal_channels =
        read_channels(config.channels_file, config.completion_channel);
    subscribe_literals(*client, literal_channels);
  }
  const auto subscribe_finished = Clock::now();
  write_ready_file(config.ready_file);

  std::uint64_t records = 0;
  std::uint64_t payload_bytes = 0;
  Clock::time_point first_message{};
  auto last_progress = Clock::now();
  Clock::time_point completion_received{};

  for (;;) {
    auto message = client->read_pubsub(std::chrono::minutes(5));
    const bool wildcard = config.mode == Mode::wildcard;
    const auto expected_kind =
        wildcard ? PubSubKind::pattern_message : PubSubKind::message;
    if (message.kind != expected_kind) {
      fail("unexpected Pub/Sub message kind while consuming");
    }
    if (wildcard && message.pattern != "*") {
      fail("wildcard delivery carried the wrong pattern");
    }
    if (message.channel == config.completion_channel) {
      const auto expected_records =
          parse_u64(message.payload, "completion payload");
      if (expected_records != records) {
        fail("completion marker says " + std::to_string(expected_records) +
             " records, but consumer received " + std::to_string(records));
      }
      completion_received = Clock::now();
      break;
    }
    if (records == 0) {
      first_message = Clock::now();
    }
    ++records;
    payload_bytes += message.payload.size();
    if (config.progress_every != 0 &&
        records % config.progress_every == 0) {
      const auto now = Clock::now();
      std::cerr << "consumed=" << records
                << " interval_seconds=" << seconds_between(last_progress, now)
                << '\n';
      last_progress = now;
    }
  }

  const auto unsubscribe_started = Clock::now();
  std::vector<goblin::core::PubSubMessage> acknowledgements;
  if (config.mode == Mode::wildcard) {
    acknowledgements = client->punsubscribe({}, std::chrono::minutes(5));
  } else {
    // Deliberately use Redis' no-argument form: this measures the server's
    // whole-client cleanup path rather than spreading work over client batches.
    acknowledgements = client->unsubscribe({}, std::chrono::minutes(5));
  }
  const auto unsubscribe_finished = Clock::now();

  const std::size_t expected_acknowledgements =
      config.mode == Mode::wildcard ? 1 : literal_channels.size();
  if (acknowledgements.size() != expected_acknowledgements) {
    fail("unsubscribe returned " + std::to_string(acknowledgements.size()) +
         " acknowledgements; expected " +
         std::to_string(expected_acknowledgements));
  }
  if (acknowledgements.empty() ||
      acknowledgements.back().subscription_count != 0) {
    fail("unsubscribe did not finish at zero subscriptions");
  }

  const double consume_seconds =
      records == 0 ? 0.0 : seconds_between(first_message, completion_received);
  std::cout
      << "{\"mode\":\""
      << (config.mode == Mode::wildcard ? "psubscribe-star"
                                        : "literal-channel-table")
      << "\",\"subscriptions\":" << expected_acknowledgements
      << ",\"records\":" << records << ",\"payload_bytes\":" << payload_bytes
      << ",\"subscribe_seconds\":"
      << seconds_between(subscribe_started, subscribe_finished)
      << ",\"consume_seconds\":" << consume_seconds
      << ",\"records_per_second\":"
      << (consume_seconds == 0.0 ? 0.0 : records / consume_seconds)
      << ",\"unsubscribe_seconds\":"
      << seconds_between(unsubscribe_started, unsubscribe_finished) << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    run(parse_args(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pubsub_jsonl_consumer: " << error.what() << '\n';
    return 1;
  }
}
