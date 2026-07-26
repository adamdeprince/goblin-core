#include "goblin/core/sbe_ring_client.hpp"

#include <simdjson.h>

#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;
using goblin::core::SbeRingClient;

struct Config {
  std::string ring;
  std::string input;
  std::string completion_channel;
  std::uint64_t progress_every{10'000'000};
  long long expected_subscribers{1};
  bool evict_file_cache{false};
};

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd) : fd_(fd) {}
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  ~FileDescriptor() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  int fd_;
};

class Mapping {
 public:
  Mapping(const char* data, std::size_t size) : data_(data), size_(size) {}
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  ~Mapping() {
    if (data_ != MAP_FAILED) {
      ::munmap(const_cast<char*>(data_), size_);
    }
  }

  [[nodiscard]] const char* data() const noexcept { return data_; }

 private:
  const char* data_;
  std::size_t size_;
};

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

[[nodiscard]] std::string errno_message(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

[[nodiscard]] std::uint64_t parse_u64(std::string_view text,
                                      std::string_view option) {
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    fail("invalid value for " + std::string(option) + ": " +
         std::string(text));
  }
  return value;
}

[[nodiscard]] long long parse_i64(std::string_view text,
                                  std::string_view option) {
  long long value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    fail("invalid value for " + std::string(option) + ": " +
         std::string(text));
  }
  return value;
}

[[nodiscard]] std::string_view next_arg(int argc, char** argv, int& index,
                                        std::string_view option) {
  if (++index >= argc) {
    fail(std::string(option) + " requires a value");
  }
  return argv[index];
}

[[nodiscard]] Config parse_args(int argc, char** argv) {
  Config config;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--ring") {
      config.ring = next_arg(argc, argv, index, option);
    } else if (option == "--input") {
      config.input = next_arg(argc, argv, index, option);
    } else if (option == "--completion-channel") {
      config.completion_channel = next_arg(argc, argv, index, option);
    } else if (option == "--expected-subscribers") {
      config.expected_subscribers =
          parse_i64(next_arg(argc, argv, index, option), option);
    } else if (option == "--progress-every") {
      config.progress_every =
          parse_u64(next_arg(argc, argv, index, option), option);
    } else if (option == "--evict-file-cache") {
      config.evict_file_cache = true;
    } else if (option == "--help") {
      std::cout
          << "Usage: " << argv[0]
          << " --ring PATH --input FILE --completion-channel CHANNEL\n"
             "       [--expected-subscribers N] [--progress-every N]\n"
             "       [--evict-file-cache]\n";
      std::exit(0);
    } else {
      fail("unknown option: " + std::string(option));
    }
  }
  if (config.ring.empty() || config.input.empty() ||
      config.completion_channel.empty()) {
    fail("--ring, --input, and --completion-channel are required");
  }
  if (config.expected_subscribers < 0) {
    fail("--expected-subscribers must be non-negative");
  }
  return config;
}

[[nodiscard]] double seconds_between(Clock::time_point begin,
                                     Clock::time_point end) {
  return std::chrono::duration<double>(end - begin).count();
}

void check_delivery_count(long long actual, const Config& config,
                          std::uint64_t record) {
  if (actual != config.expected_subscribers) {
    fail("PUBLISH " + std::to_string(record) + " reached " +
         std::to_string(actual) + " subscribers; expected " +
         std::to_string(config.expected_subscribers));
  }
}

void run(const Config& config) {
  auto client =
      SbeRingClient::open(config.ring.c_str(), std::chrono::seconds(30));
  if (!client) {
    fail("could not open SBE ring " + config.ring);
  }
  if (!client->ping(std::chrono::seconds(30))) {
    fail("SBE ring did not answer PING");
  }

  const int raw_fd = ::open(config.input.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_fd < 0) {
    fail(errno_message("open " + config.input));
  }
  FileDescriptor fd(raw_fd);

  struct stat metadata {};
  if (::fstat(fd.get(), &metadata) != 0) {
    fail(errno_message("fstat " + config.input));
  }
  if (metadata.st_size <= 0) {
    fail("input is empty: " + config.input);
  }
  const auto file_size_u64 = static_cast<std::uint64_t>(metadata.st_size);
  if (file_size_u64 > std::numeric_limits<std::size_t>::max()) {
    fail("input is too large to map on this platform");
  }
  const auto file_size = static_cast<std::size_t>(file_size_u64);

#if defined(POSIX_FADV_DONTNEED)
  if (config.evict_file_cache) {
    const int error = ::posix_fadvise(fd.get(), 0, metadata.st_size,
                                      POSIX_FADV_DONTNEED);
    if (error != 0) {
      fail("posix_fadvise(DONTNEED): " + std::string(std::strerror(error)));
    }
  }
  (void)::posix_fadvise(fd.get(), 0, metadata.st_size, POSIX_FADV_SEQUENTIAL);
#endif

  void* raw_mapping =
      ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd.get(), 0);
  if (raw_mapping == MAP_FAILED) {
    fail(errno_message("mmap " + config.input));
  }
  Mapping mapping(static_cast<const char*>(raw_mapping), file_size);
#if defined(MADV_SEQUENTIAL)
  (void)::madvise(raw_mapping, file_size, MADV_SEQUENTIAL);
#endif

  simdjson::ondemand::parser parser;
  simdjson::padded_string tail_scratch;
  std::string channel;
  channel.reserve(64);

  std::uint64_t records = 0;
  std::uint64_t payload_bytes = 0;
  const char* cursor = mapping.data();
  const char* const file_end = mapping.data() + file_size;
  const auto started = Clock::now();
  auto last_progress = started;

  while (cursor < file_end) {
    const auto remaining = static_cast<std::size_t>(file_end - cursor);
    const auto* newline =
        static_cast<const char*>(std::memchr(cursor, '\n', remaining));
    const char* line_end = newline == nullptr ? file_end : newline;
    if (line_end > cursor && line_end[-1] == '\r') {
      --line_end;
    }
    const auto line_size = static_cast<std::size_t>(line_end - cursor);
    if (line_size == 0) {
      fail("empty JSON record at line " + std::to_string(records + 1));
    }

    std::string_view event;
    std::string_view symbol;
    try {
      if (remaining >= line_size + simdjson::SIMDJSON_PADDING) {
        const simdjson::padded_string_view json(cursor, line_size, remaining);
        auto document = parser.iterate(json);
        event = document["ev"].get_string();
        symbol = document["sym"].get_string();
      } else {
        tail_scratch = simdjson::padded_string(cursor, line_size);
        auto document = parser.iterate(tail_scratch);
        event = document["ev"].get_string();
        symbol = document["sym"].get_string();
      }
    } catch (const simdjson::simdjson_error& error) {
      fail("simdjson error at line " + std::to_string(records + 1) + ": " +
           error.what());
    }

    channel.assign(event);
    channel.push_back(':');
    channel.append(symbol);
    const auto delivered = client->publish(
        channel, std::string_view(cursor, line_size), std::chrono::seconds(30));
    check_delivery_count(delivered, config, records + 1);

    ++records;
    payload_bytes += line_size;
    if (config.progress_every != 0 &&
        records % config.progress_every == 0) {
      const auto now = Clock::now();
      const double total_seconds = seconds_between(started, now);
      const double interval_seconds = seconds_between(last_progress, now);
      std::cerr << "published=" << records
                << " total_mps=" << (records / total_seconds / 1'000'000.0)
                << " interval_seconds=" << interval_seconds << '\n';
      last_progress = now;
    }
    cursor = newline == nullptr ? file_end : newline + 1;
  }

  const auto finished = Clock::now();
  const double seconds = seconds_between(started, finished);

  const auto completion_payload = std::to_string(records);
  check_delivery_count(
      client->publish(config.completion_channel, completion_payload,
                      std::chrono::seconds(30)),
      config, records + 1);

  const double payload_gib =
      static_cast<double>(payload_bytes) / (1024.0 * 1024.0 * 1024.0);
  std::cout << "{\"records\":" << records << ",\"file_bytes\":"
            << file_size_u64 << ",\"payload_bytes\":" << payload_bytes
            << ",\"seconds\":" << seconds
            << ",\"records_per_second\":" << (records / seconds)
            << ",\"payload_gib_per_second\":" << (payload_gib / seconds)
            << ",\"ring\":\"" << config.ring
            << "\",\"file_cache_advisory\":\""
            << (config.evict_file_cache ? "evicted" : "unchanged") << "\"}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    run(parse_args(argc, argv));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pubsub_jsonl_publisher: " << error.what() << '\n';
    return 1;
  }
}
