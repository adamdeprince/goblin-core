// Native workload runner for list_benchmark.py's Goblin SBE/ring row.
//
// The Python harness owns server lifecycle, memory sampling, correctness checks,
// and report generation. This helper opens an already-running shared-memory ring,
// generates one timed LIST phase in C++, and prints machine-readable counters.

#include "goblin/core/sbe_ring_client.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using goblin::core::SbeListImplementation;
using goblin::core::SbeRingClient;

struct Config {
  std::string ring;
  std::string action;
  std::string key;
  std::string operation;
  std::string push;
  std::string pop;
  std::string prefix;
  std::int64_t members = 100'000;
  std::int64_t requests = 200'000;
  std::int64_t pairs = 50'000;
  std::int64_t index = 0;
  std::int64_t start = 0;
  std::int64_t stop = -1;
  int value_bytes = 16;
  int batch = 128;
  int pipeline = 256;
  int width = 8;
  SbeListImplementation implementation = SbeListImplementation::selected;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

std::string next_arg(int argc, char** argv, int& index) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + argv[index]);
  return argv[++index];
}

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--ring") config.ring = next_arg(argc, argv, i);
    else if (arg == "--action") config.action = next_arg(argc, argv, i);
    else if (arg == "--key") config.key = next_arg(argc, argv, i);
    else if (arg == "--operation") config.operation = next_arg(argc, argv, i);
    else if (arg == "--push") config.push = next_arg(argc, argv, i);
    else if (arg == "--pop") config.pop = next_arg(argc, argv, i);
    else if (arg == "--prefix") config.prefix = next_arg(argc, argv, i);
    else if (arg == "--members") config.members = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--requests") config.requests = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--pairs") config.pairs = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--index") config.index = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--start") config.start = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--stop") config.stop = std::stoll(next_arg(argc, argv, i));
    else if (arg == "--value-bytes") config.value_bytes = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--batch") config.batch = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--pipeline") config.pipeline = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--width") config.width = std::stoi(next_arg(argc, argv, i));
    else if (arg == "--implementation") {
      const auto value = next_arg(argc, argv, i);
      if (value == "selected") config.implementation = SbeListImplementation::selected;
      else if (value == "pma") config.implementation = SbeListImplementation::pma;
      else if (value == "segmented") config.implementation = SbeListImplementation::segmented;
      else fail("invalid --implementation");
    } else {
      fail("unknown argument: " + arg);
    }
  }
  if (config.ring.empty() || config.action.empty() || config.key.empty()) {
    fail("--ring, --action, and --key are required");
  }
  if (config.members <= 0 || config.requests <= 0 || config.pairs <= 0 ||
      config.value_bytes < 2 || config.batch <= 0 || config.pipeline <= 0 ||
      config.width <= 0) {
    fail("counts and widths are out of range");
  }
  return config;
}

std::string item_value(std::int64_t item_id, int width,
                       std::string_view prefix = {}) {
  if (prefix.size() >= static_cast<std::size_t>(width)) {
    fail("value prefix must be shorter than --value-bytes");
  }
  char digits[32];
  const auto converted = std::to_chars(std::begin(digits), std::end(digits),
                                       item_id, 16);
  if (converted.ec != std::errc{}) fail("value formatting failed");
  const auto digit_count = static_cast<std::size_t>(converted.ptr - digits);
  const auto available = static_cast<std::size_t>(width) - prefix.size();
  if (digit_count > available) fail("item id does not fit --value-bytes");
  std::string result(prefix);
  result.append(available - digit_count, '0');
  result.append(digits, converted.ptr);
  return result;
}

template <class Message>
void enqueue_push(SbeRingClient& client, const Config& config,
                  std::span<const std::string_view> values) {
  std::size_t need = config.key.size();
  for (const auto value : values) need += value.size();
  client.enqueue_sbe<Message>(need, [&](auto& message) {
    message.implementation(static_cast<std::uint8_t>(config.implementation));
    message.onlyIfExists(0);
    auto& group = message.valuesCount(static_cast<std::uint16_t>(values.size()));
    for (const auto value : values) {
      group.next().putValue(value.data(), static_cast<std::uint32_t>(value.size()));
    }
    message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
  });
}

template <class Message>
void enqueue_pop(SbeRingClient& client, const Config& config, long long count) {
  client.enqueue_sbe<Message>(config.key.size(), [&](auto& message) {
    message.count(count);
    message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
  });
}

std::int64_t run_load(SbeRingClient& client, const Config& config) {
  const auto commands = (config.members + config.batch - 1) / config.batch;
  client.pipeline_for(
      static_cast<std::size_t>(commands), static_cast<std::size_t>(config.pipeline),
      [&](std::size_t command_index) {
        const auto begin = static_cast<std::int64_t>(command_index) * config.batch;
        const auto count = std::min<std::int64_t>(config.batch, config.members - begin);
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(count));
        for (std::int64_t offset = 0; offset < count; ++offset) {
          values.push_back(item_value(begin + offset, config.value_bytes));
        }
        std::vector<std::string_view> views(values.begin(), values.end());
        enqueue_push<goblin_sbe::RPush>(client, config, views);
      },
      [&](std::size_t) { (void)client.read_pipeline_int(); });
  return commands;
}

std::int64_t run_fixed(SbeRingClient& client, const Config& config) {
  const std::string replacement(static_cast<std::size_t>(config.value_bytes), 's');
  client.pipeline_for(
      static_cast<std::size_t>(config.requests),
      static_cast<std::size_t>(config.pipeline),
      [&](std::size_t) {
        if (config.operation == "LLEN") {
          client.enqueue_sbe<goblin_sbe::LLen>(config.key.size(), [&](auto& message) {
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
          });
        } else if (config.operation == "LINDEX") {
          client.enqueue_sbe<goblin_sbe::LIndex>(config.key.size(), [&](auto& message) {
            message.index(config.index);
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
          });
        } else if (config.operation == "LRANGE") {
          client.enqueue_sbe<goblin_sbe::LRange>(config.key.size(), [&](auto& message) {
            message.start(config.start).stop(config.stop);
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
          });
        } else if (config.operation == "LSET") {
          client.enqueue_sbe<goblin_sbe::LSet>(config.key.size() + replacement.size(),
                                               [&](auto& message) {
            message.index(config.index);
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
            message.putValue(replacement.data(),
                             static_cast<std::uint32_t>(replacement.size()));
          });
        } else {
          fail("unsupported fixed operation: " + config.operation);
        }
      },
      [&](std::size_t) {
        if (config.operation == "LLEN") (void)client.read_pipeline_int();
        else if (config.operation == "LINDEX") {
          (void)client.read_pipeline_bulk_or_nil();
        } else if (config.operation == "LRANGE") {
          (void)client.read_pipeline_array();
        } else {
          (void)client.read_pipeline_status();
        }
      });
  return config.requests;
}

std::int64_t run_middle(SbeRingClient& client, const Config& config) {
  const auto pivot = client.lindex(config.key, config.index);
  if (!pivot) fail("middle pivot is missing");
  const auto commands = config.pairs * 2;
  client.pipeline_for(
      static_cast<std::size_t>(commands), static_cast<std::size_t>(config.pipeline),
      [&](std::size_t command_index) {
        const auto pair = static_cast<std::int64_t>(command_index / 2);
        const auto value = item_value(pair, config.value_bytes, "m");
        if (command_index % 2 == 0) {
          client.enqueue_sbe<goblin_sbe::LInsert>(
              config.key.size() + pivot->size() + value.size(), [&](auto& message) {
            message.before(1);
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
            message.putPivot(pivot->data(), static_cast<std::uint32_t>(pivot->size()));
            message.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
          });
        } else {
          client.enqueue_sbe<goblin_sbe::LRem>(config.key.size() + value.size(),
                                               [&](auto& message) {
            message.count(1);
            message.putKey(config.key.data(), static_cast<std::uint32_t>(config.key.size()));
            message.putValue(value.data(), static_cast<std::uint32_t>(value.size()));
          });
        }
      },
      [&](std::size_t command_index) {
        const auto reply = client.read_pipeline_int();
        if (command_index % 2 != 0 && reply != 1) {
          fail("middle LREM missed inserted value");
        }
      });
  return commands;
}

std::int64_t run_endpoint(SbeRingClient& client, const Config& config) {
  const auto commands = config.pairs * 2;
  const bool stack = (config.push == "LPUSH" && config.pop == "LPOP") ||
                     (config.push == "RPUSH" && config.pop == "RPOP");
  client.pipeline_for(
      static_cast<std::size_t>(commands), static_cast<std::size_t>(config.pipeline),
      [&](std::size_t command_index) {
        const auto pair = static_cast<std::int64_t>(command_index / 2);
        const auto value = item_value(pair, config.value_bytes, config.prefix);
        if (command_index % 2 == 0) {
          const std::string_view view = value;
          if (config.push == "LPUSH") {
            enqueue_push<goblin_sbe::LPush>(client, config,
                                            std::span<const std::string_view>(&view, 1));
          } else if (config.push == "RPUSH") {
            enqueue_push<goblin_sbe::RPush>(client, config,
                                            std::span<const std::string_view>(&view, 1));
          } else {
            fail("unsupported push: " + config.push);
          }
        } else if (config.pop == "LPOP") {
          enqueue_pop<goblin_sbe::LPop>(client, config, -1);
        } else if (config.pop == "RPOP") {
          enqueue_pop<goblin_sbe::RPop>(client, config, -1);
        } else {
          fail("unsupported pop: " + config.pop);
        }
      },
      [&](std::size_t command_index) {
        if (command_index % 2 == 0) {
          (void)client.read_pipeline_int();
        } else {
          const auto popped = client.read_pipeline_bulk_or_nil();
          if (!popped) fail("endpoint pop returned nil");
          if (stack) {
            const auto pair = static_cast<std::int64_t>(command_index / 2);
            const auto expected = item_value(pair, config.value_bytes, config.prefix);
            if (*popped != expected) fail("stack pop returned the wrong value");
          }
        }
      });
  return commands;
}

std::int64_t run_counted(SbeRingClient& client, const Config& config) {
  const auto commands = config.pairs * 2;
  client.pipeline_for(
      static_cast<std::size_t>(commands), static_cast<std::size_t>(config.pipeline),
      [&](std::size_t command_index) {
        const auto pair = static_cast<std::int64_t>(command_index / 2);
        if (command_index % 2 == 0) {
          std::vector<std::string> values;
          values.reserve(static_cast<std::size_t>(config.width));
          for (int offset = 0; offset < config.width; ++offset) {
            values.push_back(item_value(pair * config.width + offset,
                                        config.value_bytes, "b"));
          }
          std::vector<std::string_view> views(values.begin(), values.end());
          enqueue_push<goblin_sbe::LPush>(client, config, views);
        } else {
          enqueue_pop<goblin_sbe::LPop>(client, config, config.width);
        }
      },
      [&](std::size_t command_index) {
        if (command_index % 2 == 0) {
          (void)client.read_pipeline_int();
        } else {
          const auto popped = client.read_pipeline_array_or_nil();
          if (!popped || popped->size() != static_cast<std::size_t>(config.width)) {
            fail("counted LPOP returned the wrong number of values");
          }
        }
      });
  return commands;
}

int run(int argc, char** argv) {
  const Config config = parse_args(argc, argv);
  auto client = SbeRingClient::open(config.ring.c_str(), std::chrono::seconds(10));
  if (!client) fail("could not open SBE ring");

  const auto started = Clock::now();
  std::int64_t commands = 0;
  if (config.action == "load") commands = run_load(*client, config);
  else if (config.action == "fixed") commands = run_fixed(*client, config);
  else if (config.action == "middle") commands = run_middle(*client, config);
  else if (config.action == "endpoint") commands = run_endpoint(*client, config);
  else if (config.action == "counted") commands = run_counted(*client, config);
  else fail("unsupported action: " + config.action);
  const double seconds = std::chrono::duration<double>(Clock::now() - started).count();

  std::cout << "commands=" << commands << '\n'
            << std::setprecision(17) << "seconds=" << seconds << '\n';
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "list_sbe_benchmark: " << error.what() << '\n';
    return 1;
  }
}
