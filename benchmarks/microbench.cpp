#include "goblin/core/command.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/score_format.hpp"
#include "goblin/core/store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t benchmark_sink = 0;

enum class ScoreShape {
  integer,
  short_decimal,
  long_decimal,
  random_double,
};

struct Options {
  std::size_t members{1'000'000};
  std::size_t ops{1'000'000};
  std::size_t range_size{16};
  std::size_t warmups{1};
  std::uint64_t seed{12'345};
  ScoreShape score_shape{ScoreShape::integer};
  goblin::core::RankCacheMode rank_cache_mode{goblin::core::RankCacheMode::Off};
  bool score_string_cache{false};
  std::string format{"markdown"};
  std::string output;
  std::vector<std::string> categories;
  std::vector<std::string> metrics;
  bool list_benchmarks{false};
};

struct BenchmarkResult {
  std::string category;
  std::string metric;
  std::size_t operations{0};
  double seconds{0.0};
  double ns_per_op{0.0};
  double ops_per_second{0.0};
  std::uint64_t checksum{0};
};

struct RangeText {
  std::string start;
  std::string stop;
};

struct Fixture {
  static constexpr std::string_view key = "leaders";

  std::vector<std::string> members;
  std::vector<double> scores;
  std::vector<std::string> score_texts;
  std::vector<std::size_t> lookup_ids;
  std::vector<std::size_t> range_starts;
  std::vector<RangeText> range_texts;
  std::vector<goblin::core::ZSetEntry> sorted_entries;
  std::vector<goblin::core::ZSetScoreEntry> sorted_score_entries;
  goblin::core::ZSetMemberStorage member_storage;
  goblin::core::ZSetScoreIndex score_index;
  goblin::core::ZSet zset;
  goblin::core::Store store;
};

struct ScoreEntryLess {
  const goblin::core::ZSetMemberStorage* members{nullptr};

  [[nodiscard]] bool operator()(goblin::core::ZSetScoreEntry lhs,
                                goblin::core::ZSetScoreEntry rhs) const noexcept {
    if (lhs.score < rhs.score) {
      return true;
    }
    if (lhs.score > rhs.score) {
      return false;
    }
    return members->view(lhs.member_id) < members->view(rhs.member_id);
  }
};

[[nodiscard]] std::string member_for(std::size_t member_id) {
  auto number = std::to_string(member_id);
  if (number.size() < 10) {
    number.insert(number.begin(), 10 - number.size(), '0');
  }
  return "member:" + number;
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
  value += 0x9E37'79B9'7F4A'7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] std::string_view score_shape_name(ScoreShape shape) noexcept {
  switch (shape) {
    case ScoreShape::integer:
      return "integer";
    case ScoreShape::short_decimal:
      return "short-decimal";
    case ScoreShape::long_decimal:
      return "long-decimal";
    case ScoreShape::random_double:
      return "random-double";
  }
  return "integer";
}

[[nodiscard]] std::optional<ScoreShape> parse_score_shape(std::string_view text) {
  if (text == "integer") {
    return ScoreShape::integer;
  }
  if (text == "short-decimal") {
    return ScoreShape::short_decimal;
  }
  if (text == "long-decimal") {
    return ScoreShape::long_decimal;
  }
  if (text == "random-double") {
    return ScoreShape::random_double;
  }
  return std::nullopt;
}

[[nodiscard]] double integer_score_for(std::size_t member_id) noexcept {
  const auto value =
      (static_cast<std::uint64_t>(member_id) * 1'103'515'245ULL + 12'345ULL) &
      0xFFFF'FFFFULL;
  return static_cast<double>(value);
}

[[nodiscard]] double score_for(std::size_t member_id,
                               ScoreShape shape,
                               std::uint64_t seed) noexcept {
  const auto hashed =
      splitmix64(static_cast<std::uint64_t>(member_id) ^ (seed << 1U));
  switch (shape) {
    case ScoreShape::integer:
      return integer_score_for(member_id);
    case ScoreShape::short_decimal:
      return static_cast<double>(hashed % 1'000'000'000ULL) / 100.0;
    case ScoreShape::long_decimal: {
      const auto whole = hashed % 10'000'000ULL;
      const auto fraction = splitmix64(hashed) % 1'000'000'000'000ULL;
      return static_cast<double>(whole) +
             static_cast<double>(fraction) / 1'000'000'000'000.0;
    }
    case ScoreShape::random_double: {
      constexpr double kInvTwoPow53 = 1.0 / 9'007'199'254'740'992.0;
      const auto mantissa = hashed >> 11U;
      return static_cast<double>(mantissa) * kInvTwoPow53 * 1'000'000'000.0;
    }
  }
  return integer_score_for(member_id);
}

[[nodiscard]] std::vector<std::size_t> shuffled_ids(std::size_t count,
                                                    std::uint64_t seed) {
  std::vector<std::size_t> ids(count);
  for (std::size_t i = 0; i < count; ++i) {
    ids[i] = i;
  }
  std::mt19937_64 rng(seed);
  std::shuffle(ids.begin(), ids.end(), rng);
  return ids;
}

[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view text) {
  std::size_t value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) {
  std::uint64_t value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<goblin::core::RankCacheMode> parse_rank_cache_mode(
    std::string_view text) {
  if (text == "off" || text == "none") {
    return goblin::core::RankCacheMode::Off;
  }
  if (text == "exact" || text == "location") {
    return goblin::core::RankCacheMode::Exact;
  }
  if (text == "block-hint" || text == "block") {
    return goblin::core::RankCacheMode::BlockHint;
  }
  return std::nullopt;
}

void print_usage(std::ostream& out, std::string_view program) {
  out << "usage: " << program
      << " [--members N] [--ops N] [--range-size N] [--warmups N]\n"
      << "       [--seed N] [--rank-cache|--no-rank-cache]\n"
      << "       [--rank-cache-mode off|exact|block-hint]\n"
      << "       [--score-shape integer|short-decimal|long-decimal|random-double]\n"
      << "       [--score-string-cache|--no-score-string-cache]\n"
      << "       [--format markdown|json|csv] [--output PATH]\n"
      << "       [--category NAME]... [--metric NAME]...\n"
      << "       [--list-benchmarks]\n";
}

[[nodiscard]] bool parse_args(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    auto need_value = [&](std::string_view name) -> std::optional<std::string_view> {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << '\n';
        return std::nullopt;
      }
      return std::string_view(argv[++i]);
    };

    if (arg == "--help" || arg == "-h") {
      print_usage(std::cout, argv[0]);
      std::exit(0);
    }
    if (arg == "--members") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_size(*text);
      if (!value || *value == 0) {
        std::cerr << "--members must be a positive integer\n";
        return false;
      }
      options.members = *value;
      continue;
    }
    if (arg == "--ops") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_size(*text);
      if (!value || *value == 0) {
        std::cerr << "--ops must be a positive integer\n";
        return false;
      }
      options.ops = *value;
      continue;
    }
    if (arg == "--range-size") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_size(*text);
      if (!value || *value == 0) {
        std::cerr << "--range-size must be a positive integer\n";
        return false;
      }
      options.range_size = *value;
      continue;
    }
    if (arg == "--warmups") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_size(*text);
      if (!value) {
        std::cerr << "--warmups must be a non-negative integer\n";
        return false;
      }
      options.warmups = *value;
      continue;
    }
    if (arg == "--seed") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_u64(*text);
      if (!value) {
        std::cerr << "--seed must be a non-negative integer\n";
        return false;
      }
      options.seed = *value;
      continue;
    }
    if (arg == "--score-shape") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_score_shape(*text);
      if (!value) {
        std::cerr << "--score-shape must be integer, short-decimal, "
                     "long-decimal, or random-double\n";
        return false;
      }
      options.score_shape = *value;
      continue;
    }
    if (arg == "--rank-cache") {
      options.rank_cache_mode = goblin::core::RankCacheMode::Exact;
      continue;
    }
    if (arg == "--no-rank-cache") {
      options.rank_cache_mode = goblin::core::RankCacheMode::Off;
      continue;
    }
    if (arg == "--rank-cache-mode") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      const auto value = parse_rank_cache_mode(*text);
      if (!value) {
        std::cerr << "--rank-cache-mode must be off, exact, or block-hint\n";
        return false;
      }
      options.rank_cache_mode = *value;
      continue;
    }
    if (arg == "--score-string-cache") {
      options.score_string_cache = true;
      continue;
    }
    if (arg == "--no-score-string-cache") {
      options.score_string_cache = false;
      continue;
    }
    if (arg == "--format") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      options.format = std::string(*text);
      if (options.format != "markdown" && options.format != "json" &&
          options.format != "csv") {
        std::cerr << "--format must be markdown, json, or csv\n";
        return false;
      }
      continue;
    }
    if (arg == "--output") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      options.output = std::string(*text);
      continue;
    }

    if (arg == "--category") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      options.categories.emplace_back(*text);
      continue;
    }
    if (arg == "--metric") {
      const auto text = need_value(arg);
      if (!text) {
        return false;
      }
      options.metrics.emplace_back(*text);
      continue;
    }
    if (arg == "--list-benchmarks") {
      options.list_benchmarks = true;
      continue;
    }
    std::cerr << "unknown option: " << arg << '\n';
    return false;
  }

  if (options.range_size > options.members) {
    std::cerr << "--range-size must be less than or equal to --members\n";
    return false;
  }
  return true;
}

void consume(std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "r,m"(value) : "memory");
#endif
  benchmark_sink = benchmark_sink ^ value;
}

[[nodiscard]] std::uint64_t mix(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  lhs ^= rhs + 0x9E37'79B9'7F4A'7C15ULL + (lhs << 6U) + (lhs >> 2U);
  return lhs;
}

[[nodiscard]] std::uint64_t digest_output(std::string_view out) noexcept {
  std::uint64_t value = out.size();
  if (!out.empty()) {
    value = mix(value, static_cast<unsigned char>(out.front()));
    value = mix(value, static_cast<unsigned char>(out.back()));
  }
  return value;
}

[[nodiscard]] std::uint64_t digest_entry(const goblin::core::ZSetEntry& entry) noexcept {
  auto value = static_cast<std::uint64_t>(entry.member.size());
  value = mix(value, static_cast<std::uint64_t>(entry.score));
  if (!entry.member.empty()) {
    value = mix(value, static_cast<unsigned char>(entry.member.front()));
    value = mix(value, static_cast<unsigned char>(entry.member.back()));
  }
  return value;
}

[[nodiscard]] std::uint64_t digest_score_entry(
    const goblin::core::ZSetScoreEntry& entry) noexcept {
  auto value = static_cast<std::uint64_t>(entry.member_id);
  value = mix(value, static_cast<std::uint64_t>(entry.score));
  return value;
}

[[nodiscard]] std::uint64_t digest_score_text(double score) noexcept {
  std::array<char, 32> buffer;
  auto text = goblin::core::score_format::try_format_finite_to_buffer(score, buffer);
  std::string fallback;
  if (text.empty()) {
    fallback = goblin::core::score_format::fallback(score);
    text = fallback;
  }

  const auto size = static_cast<std::uint64_t>(text.size());
  auto value = size;
  if (size != 0) {
    value = mix(value, static_cast<unsigned char>(text.front()));
    value = mix(value, static_cast<unsigned char>(text.back()));
  }
  return value;
}

void append_range(std::string& out,
                  std::span<const goblin::core::ZSetEntry> entries,
                  bool with_scores) {
  goblin::core::resp::reserve_append_capacity(
      out, 16 + entries.size() * (with_scores ? 48 : 32));
  goblin::core::resp::append_array_header(
      out, with_scores ? entries.size() * 2 : entries.size());
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_string(out, entry.member);
    if (with_scores) {
      if (entry.score_text.empty()) {
        goblin::core::resp::append_bulk_finite_double(out, entry.score);
      } else {
        goblin::core::resp::append_bulk_string(out, entry.score_text);
      }
    }
  }
}

void append_member_range_direct(
    std::string& out,
    std::span<const goblin::core::ZSetScoreEntry> entries,
    const goblin::core::ZSetMemberStorage& members) {
  goblin::core::resp::reserve_append_capacity(out, 16 + entries.size() * 32);
  goblin::core::resp::append_array_header(out, entries.size());
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_string(out, members.view(entry.member_id));
  }
}

void append_preformatted_score_range(
    std::string& out,
    std::span<const goblin::core::ZSetScoreEntry> entries,
    std::span<const std::string> score_texts) {
  goblin::core::resp::reserve_append_capacity(out, 16 + entries.size() * 16);
  goblin::core::resp::append_array_header(out, entries.size());
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_string(out, score_texts[entry.member_id]);
  }
}

void append_formatting_score_range(
    std::string& out,
    std::span<const goblin::core::ZSetScoreEntry> entries) {
  goblin::core::resp::reserve_append_capacity(out, 16 + entries.size() * 16);
  goblin::core::resp::append_array_header(out, entries.size());
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_finite_double(out, entry.score);
  }
}

void append_member_score_range_direct(
    std::string& out,
    std::span<const goblin::core::ZSetScoreEntry> entries,
    const goblin::core::ZSetMemberStorage& members) {
  goblin::core::resp::reserve_append_capacity(out, 16 + entries.size() * 48);
  goblin::core::resp::append_array_header(out, entries.size() * 2);
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_string(out, members.view(entry.member_id));
    goblin::core::resp::append_bulk_finite_double(out, entry.score);
  }
}

[[nodiscard]] std::string serialize_range(
    std::span<const goblin::core::ZSetEntry> entries,
    bool with_scores) {
  std::string out;
  append_range(out, entries, with_scores);
  return out;
}

[[nodiscard]] Fixture build_fixture(const Options& options) {
  Fixture fixture{
      .zset = goblin::core::ZSet(
          goblin::core::ZSetOptions{
              .rank_cache_mode = options.rank_cache_mode,
              .score_string_cache = options.score_string_cache,
          }),
      .store = goblin::core::Store(
          goblin::core::StoreOptions{
              .rank_cache_mode = options.rank_cache_mode,
              .score_string_cache = options.score_string_cache,
          }),
  };
  fixture.members.reserve(options.members);
  fixture.scores.reserve(options.members);
  fixture.score_texts.reserve(options.members);
  for (std::size_t i = 0; i < options.members; ++i) {
    fixture.members.push_back(member_for(i));
    fixture.scores.push_back(score_for(i, options.score_shape, options.seed));
    fixture.score_texts.push_back(goblin::core::format_score(fixture.scores.back()));
  }

  fixture.member_storage.reserve(options.members);
  fixture.score_index.set_members(&fixture.member_storage);
  std::vector<goblin::core::ZSetScoreEntry> score_entries;
  score_entries.reserve(options.members);
  for (std::size_t i = 0; i < options.members; ++i) {
    const auto member_id =
        fixture.member_storage.push_back(fixture.members[i], fixture.scores[i]);
    score_entries.push_back(
        goblin::core::ZSetScoreEntry{.score = fixture.scores[i],
                                     .member_id = member_id});
  }
  std::sort(score_entries.begin(), score_entries.end(),
            ScoreEntryLess{.members = &fixture.member_storage});
  fixture.score_index.assign_sorted(score_entries);

  const auto load_ids = shuffled_ids(options.members, options.seed);
  for (const auto member_id : load_ids) {
    (void)fixture.zset.add(fixture.scores[member_id], fixture.members[member_id]);
    (void)fixture.store.zadd(Fixture::key, fixture.scores[member_id],
                             fixture.members[member_id]);
  }

  fixture.lookup_ids.reserve(options.ops);
  for (std::size_t i = 0; i < options.ops; ++i) {
    fixture.lookup_ids.push_back(load_ids[i % load_ids.size()]);
  }
  std::mt19937_64 lookup_rng(options.seed + 1);
  std::shuffle(fixture.lookup_ids.begin(), fixture.lookup_ids.end(), lookup_rng);

  fixture.range_starts.reserve(options.ops);
  fixture.range_texts.reserve(options.ops);
  std::mt19937_64 range_rng(options.seed + 2);
  std::uniform_int_distribution<std::size_t> range_start_dist(
      0, options.members - options.range_size);
  for (std::size_t i = 0; i < options.ops; ++i) {
    const auto start = range_start_dist(range_rng);
    const auto stop = start + options.range_size - 1;
    fixture.range_starts.push_back(start);
    fixture.range_texts.push_back(
        RangeText{.start = std::to_string(start), .stop = std::to_string(stop)});
  }

  fixture.sorted_entries = fixture.zset.range(0, static_cast<long long>(options.members) - 1);
  fixture.sorted_score_entries = fixture.score_index.range(0, options.members);
  return fixture;
}

template <class Fn>
[[nodiscard]] BenchmarkResult measure(const Options& options,
                                      std::string category,
                                      std::string metric,
                                      std::size_t operations,
                                      Fn&& fn) {
  for (std::size_t i = 0; i < options.warmups; ++i) {
    consume(fn());
  }

  const auto started = Clock::now();
  const auto checksum = fn();
  const auto stopped = Clock::now();
  consume(checksum);

  const auto seconds =
      std::chrono::duration<double>(stopped - started).count();
  return BenchmarkResult{
      .category = std::move(category),
      .metric = std::move(metric),
      .operations = operations,
      .seconds = seconds,
      .ns_per_op = seconds * 1'000'000'000.0 / static_cast<double>(operations),
      .ops_per_second = static_cast<double>(operations) / seconds,
      .checksum = checksum,
  };
}

[[nodiscard]] goblin::core::Command make_command(
    goblin::core::CommandType type,
    std::string_view name,
    std::span<const std::string_view> args,
    bool with_scores = false) {
  goblin::core::Command command;
  command.type = type;
  command.name = name;
  command.args = args;
  command.with_scores = with_scores;
  if ((type == goblin::core::CommandType::zrange ||
       type == goblin::core::CommandType::zrevrange) &&
      args.size() >= 3) {
    long long start = 0;
    long long stop = 0;
    const auto* start_begin = args[1].data();
    const auto* start_end = start_begin + args[1].size();
    const auto* stop_begin = args[2].data();
    const auto* stop_end = stop_begin + args[2].size();
    const auto [start_ptr, start_ec] =
        std::from_chars(start_begin, start_end, start);
    const auto [stop_ptr, stop_ec] =
        std::from_chars(stop_begin, stop_end, stop);
    if (start_ec == std::errc{} && start_ptr == start_end &&
        stop_ec == std::errc{} && stop_ptr == stop_end) {
      command.range_indexes_parsed = true;
      command.range_start = start;
      command.range_stop = stop;
    }
  }
  return command;
}


[[nodiscard]] bool benchmark_selected(const Options& options,
                                      std::string_view category,
                                      std::string_view metric) noexcept {
  if (!options.categories.empty()) {
    bool matched = false;
    for (const auto& selected : options.categories) {
      if (selected == category) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  if (!options.metrics.empty()) {
    bool matched = false;
    for (const auto& selected : options.metrics) {
      if (selected == metric) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }
  return true;
}

void list_benchmark_catalog(std::ostream& out) {
  static constexpr std::pair<std::string_view, std::string_view> catalog[] = {
      {"raw_zset", "zscore"},
      {"raw_zset", "zrank"},
      {"raw_zset", "zrevrank"},
      {"raw_zset", "zrange_iter"},
      {"raw_zset", "zrange_vector"},
      {"zrange_breakdown", "score_index_traversal"},
      {"zrange_breakdown", "member_lookup"},
      {"zrange_breakdown", "score_format_only"},
      {"zrange_breakdown", "member_resp_append_only"},
      {"zrange_breakdown", "score_resp_append_preformatted"},
      {"zrange_breakdown", "score_resp_append_formatting"},
      {"zrange_breakdown", "direct_resp_append_withscores"},
      {"zrange_breakdown", "resp_append"},
      {"zrange_breakdown", "resp_append_withscores"},
      {"zrange_breakdown", "execute_command_into"},
      {"zrange_breakdown", "execute_command_into_withscores"},
      {"resp", "zscore_bulk"},
      {"resp", "zrank_integer"},
      {"resp", "zrange"},
      {"resp", "zrange_withscores"},
      {"command_into", "zscore"},
      {"command_into", "zrank"},
      {"command_into", "zrevrank"},
      {"command_into", "zrange"},
      {"command_into", "zrange_withscores"},
      {"command_string", "zscore"},
      {"command_string", "zrank"},
      {"command_string", "zrevrank"},
      {"command_string", "zrange"},
      {"command_string", "zrange_withscores"},
      {"parse_command_into", "zscore"},
      {"parse_command_into", "zrank"},
      {"parse_command_into", "zrevrank"},
      {"parse_command_into", "zrange"},
      {"parse_command_into", "zrange_withscores"},
      {"parse_command_string", "zscore"},
      {"parse_command_string", "zrank"},
      {"parse_command_string", "zrevrank"},
      {"parse_command_string", "zrange"},
      {"parse_command_string", "zrange_withscores"},
  };

  for (const auto& [category, metric] : catalog) {
    out << category << '\t' << metric << '\n';
  }
}

[[nodiscard]] std::vector<BenchmarkResult> run_benchmarks(const Options& options,
                                                          Fixture& fixture) {
  std::vector<BenchmarkResult> results;
  results.reserve(48);

  const auto maybe_bench = [&](std::string_view category,
                               std::string_view metric,
                               std::size_t operations,
                               auto&& fn) {
    if (!benchmark_selected(options, category, metric)) {
      return;
    }
    results.push_back(measure(
        options,
        std::string(category),
        std::string(metric),
        operations,
        std::forward<decltype(fn)>(fn)));
  };

  maybe_bench("raw_zset", "zscore", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const auto score = fixture.zset.score(fixture.members[member_id]);
      checksum = mix(checksum, score ? static_cast<std::uint64_t>(*score) : 0);
    }
    return checksum;
  });

  maybe_bench("raw_zset", "zrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const auto rank = fixture.zset.rank(fixture.members[member_id]);
      checksum = mix(checksum, rank ? static_cast<std::uint64_t>(*rank) : 0);
    }
    return checksum;
  });

  maybe_bench("raw_zset", "zrevrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const auto rank = fixture.zset.reverse_rank(fixture.members[member_id]);
      checksum = mix(checksum, rank ? static_cast<std::uint64_t>(*rank) : 0);
    }
    return checksum;
  });

  maybe_bench("raw_zset", "zrange_iter", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      fixture.zset.for_range(
          static_cast<long long>(start),
          static_cast<long long>(start + options.range_size - 1),
          [&checksum](const goblin::core::ZSetEntry& entry) {
            checksum = mix(checksum, digest_entry(entry));
          });
    }
    return checksum;
  });

  maybe_bench("raw_zset", "zrange_vector", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      const auto entries = fixture.zset.range(
          static_cast<long long>(start),
          static_cast<long long>(start + options.range_size - 1));
      for (const auto& entry : entries) {
        checksum = mix(checksum, digest_entry(entry));
      }
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "score_index_traversal", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      auto append = [&checksum](double score, std::uint32_t member_id) {
        checksum = mix(
            checksum,
            digest_score_entry(
                goblin::core::ZSetScoreEntry{.score = score, .member_id = member_id}));
      };
      fixture.score_index.for_range(start, options.range_size, append);
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "member_lookup", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      const auto* begin = fixture.sorted_score_entries.data() + start;
      for (std::size_t i = 0; i < options.range_size; ++i) {
        const auto member = fixture.member_storage.view(begin[i].member_id);
        auto value = static_cast<std::uint64_t>(member.size());
        if (!member.empty()) {
          value = mix(value, static_cast<unsigned char>(member.front()));
          value = mix(value, static_cast<unsigned char>(member.back()));
        }
        checksum = mix(checksum, value);
      }
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "score_format_only", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      const auto* begin = fixture.sorted_score_entries.data() + start;
      for (std::size_t i = 0; i < options.range_size; ++i) {
        checksum = mix(checksum, digest_score_text(begin[i].score));
      }
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "member_resp_append_only", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_member_range_direct(
          out,
          std::span<const goblin::core::ZSetScoreEntry>(
              fixture.sorted_score_entries.data() + start, options.range_size),
          fixture.member_storage);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "score_resp_append_preformatted", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_preformatted_score_range(
          out,
          std::span<const goblin::core::ZSetScoreEntry>(
              fixture.sorted_score_entries.data() + start, options.range_size),
          fixture.score_texts);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "score_resp_append_formatting", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_formatting_score_range(
          out,
          std::span<const goblin::core::ZSetScoreEntry>(
              fixture.sorted_score_entries.data() + start, options.range_size));
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "direct_resp_append_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_member_score_range_direct(
          out,
          std::span<const goblin::core::ZSetScoreEntry>(
              fixture.sorted_score_entries.data() + start, options.range_size),
          fixture.member_storage);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "resp_append", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_range(
          out,
          std::span<const goblin::core::ZSetEntry>(
              fixture.sorted_entries.data() + start, options.range_size),
          false);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "resp_append_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto start : fixture.range_starts) {
      out.clear();
      append_range(
          out,
          std::span<const goblin::core::ZSetEntry>(
              fixture.sorted_entries.data() + start, options.range_size),
          true);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "execute_command_into", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto& text : fixture.range_texts) {
      out.clear();
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("zrange_breakdown", "execute_command_into_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto& text : fixture.range_texts) {
      out.clear();
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args, true);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("resp", "zscore_bulk", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      std::string out;
      out.reserve(32);
      goblin::core::resp::append_bulk_finite_double(out, fixture.scores[member_id]);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("resp", "zrank_integer", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < fixture.lookup_ids.size(); ++i) {
      const auto out = goblin::core::resp::integer(
          static_cast<long long>(fixture.lookup_ids[i]));
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("resp", "zrange", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      const auto out = serialize_range(
          std::span<const goblin::core::ZSetEntry>(
              fixture.sorted_entries.data() + start, options.range_size),
          false);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("resp", "zrange_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto start : fixture.range_starts) {
      const auto out = serialize_range(
          std::span<const goblin::core::ZSetEntry>(
              fixture.sorted_entries.data() + start, options.range_size),
          true);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_into", "zscore", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zscore, "ZSCORE", args);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_into", "zrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zrank, "ZRANK", args);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_into", "zrevrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zrevrank, "ZREVRANK", args);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_into", "zrange", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (std::size_t i = 0; i < fixture.range_texts.size(); ++i) {
      out.clear();
      const auto& text = fixture.range_texts[i];
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_into", "zrange_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (std::size_t i = 0; i < fixture.range_texts.size(); ++i) {
      out.clear();
      const auto& text = fixture.range_texts[i];
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args, true);
      goblin::core::execute_command_into(fixture.store, command, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_string", "zscore", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zscore, "ZSCORE", args);
      const auto out = goblin::core::execute_command(fixture.store, command);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_string", "zrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zrank, "ZRANK", args);
      const auto out = goblin::core::execute_command(fixture.store, command);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_string", "zrevrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 2> args{
          Fixture::key, std::string_view(fixture.members[member_id])};
      const auto command =
          make_command(goblin::core::CommandType::zrevrank, "ZREVRANK", args);
      const auto out = goblin::core::execute_command(fixture.store, command);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_string", "zrange", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < fixture.range_texts.size(); ++i) {
      const auto& text = fixture.range_texts[i];
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args);
      const auto out = goblin::core::execute_command(fixture.store, command);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("command_string", "zrange_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < fixture.range_texts.size(); ++i) {
      const auto& text = fixture.range_texts[i];
      const std::array<std::string_view, 3> args{
          Fixture::key, std::string_view(text.start), std::string_view(text.stop)};
      const auto command =
          make_command(goblin::core::CommandType::zrange, "ZRANGE", args, true);
      const auto out = goblin::core::execute_command(fixture.store, command);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_into", "zscore", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 3> fields{
          "ZSCORE", Fixture::key, std::string_view(fixture.members[member_id])};
      goblin::core::handle_command_into(fixture.store, fields, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_into", "zrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 3> fields{
          "ZRANK", Fixture::key, std::string_view(fixture.members[member_id])};
      goblin::core::handle_command_into(fixture.store, fields, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_into", "zrevrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto member_id : fixture.lookup_ids) {
      out.clear();
      const std::array<std::string_view, 3> fields{
          "ZREVRANK", Fixture::key, std::string_view(fixture.members[member_id])};
      goblin::core::handle_command_into(fixture.store, fields, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_into", "zrange", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto& text : fixture.range_texts) {
      out.clear();
      const std::array<std::string_view, 4> fields{
          "ZRANGE", Fixture::key, std::string_view(text.start),
          std::string_view(text.stop)};
      goblin::core::handle_command_into(fixture.store, fields, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_into", "zrange_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    std::string out;
    for (const auto& text : fixture.range_texts) {
      out.clear();
      const std::array<std::string_view, 5> fields{
          "ZRANGE", Fixture::key, std::string_view(text.start),
          std::string_view(text.stop), "WITHSCORES"};
      goblin::core::handle_command_into(fixture.store, fields, out);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_string", "zscore", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 3> fields{
          "ZSCORE", Fixture::key, std::string_view(fixture.members[member_id])};
      const auto out = goblin::core::handle_command(fixture.store, fields);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_string", "zrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 3> fields{
          "ZRANK", Fixture::key, std::string_view(fixture.members[member_id])};
      const auto out = goblin::core::handle_command(fixture.store, fields);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_string", "zrevrank", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto member_id : fixture.lookup_ids) {
      const std::array<std::string_view, 3> fields{
          "ZREVRANK", Fixture::key, std::string_view(fixture.members[member_id])};
      const auto out = goblin::core::handle_command(fixture.store, fields);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_string", "zrange", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto& text : fixture.range_texts) {
      const std::array<std::string_view, 4> fields{
          "ZRANGE", Fixture::key, std::string_view(text.start),
          std::string_view(text.stop)};
      const auto out = goblin::core::handle_command(fixture.store, fields);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  maybe_bench("parse_command_string", "zrange_withscores", options.ops, [&] {
    std::uint64_t checksum = 0;
    for (const auto& text : fixture.range_texts) {
      const std::array<std::string_view, 5> fields{
          "ZRANGE", Fixture::key, std::string_view(text.start),
          std::string_view(text.stop), "WITHSCORES"};
      const auto out = goblin::core::handle_command(fixture.store, fields);
      checksum = mix(checksum, digest_output(out));
    }
    return checksum;
  });

  return results;
}

void write_json_string(std::ostream& out, std::string_view value) {
  out << '"';
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  out << '"';
}

void write_markdown(std::ostream& out,
                    const Options& options,
                    std::span<const BenchmarkResult> results) {
  out << "# Goblin Core Microbenchmark\n\n";
  out << "- members: `" << options.members << "`\n";
  out << "- operations per metric: `" << options.ops << "`\n";
  out << "- range size: `" << options.range_size << "`\n";
  out << "- warmups: `" << options.warmups << "`\n";
  out << "- seed: `" << options.seed << "`\n";
  out << "- score shape: `" << score_shape_name(options.score_shape) << "`\n";
  out << "- rank cache: `" << rank_cache_mode_name(options.rank_cache_mode) << "`\n";
  out << "- score string cache: `"
      << (options.score_string_cache ? "on" : "off") << "`\n\n";
  out << "| Category | Metric | Ops | ns/op | ops/sec | Seconds | Checksum |\n";
  out << "| --- | --- | ---: | ---: | ---: | ---: | ---: |\n";
  out << std::fixed << std::setprecision(2);
  for (const auto& result : results) {
    out << "| `" << result.category << "` | `" << result.metric << "` | "
        << result.operations << " | " << result.ns_per_op << " | "
        << result.ops_per_second << " | " << std::setprecision(6)
        << result.seconds << std::setprecision(2) << " | " << result.checksum
        << " |\n";
  }
}

void write_csv(std::ostream& out, std::span<const BenchmarkResult> results) {
  out << "category,metric,operations,seconds,ns_per_op,ops_per_second,checksum\n";
  out << std::setprecision(17);
  for (const auto& result : results) {
    out << result.category << ',' << result.metric << ',' << result.operations << ','
        << result.seconds << ',' << result.ns_per_op << ','
        << result.ops_per_second << ',' << result.checksum << '\n';
  }
}

void write_json(std::ostream& out,
                const Options& options,
                std::span<const BenchmarkResult> results) {
  out << "{\n  \"config\": {\n";
  out << "    \"members\": " << options.members << ",\n";
  out << "    \"ops\": " << options.ops << ",\n";
  out << "    \"range_size\": " << options.range_size << ",\n";
  out << "    \"warmups\": " << options.warmups << ",\n";
  out << "    \"seed\": " << options.seed << ",\n";
  out << "    \"score_shape\": ";
  write_json_string(out, score_shape_name(options.score_shape));
  out << ",\n";
  out << "    \"rank_cache\": "
      << (options.rank_cache_mode != goblin::core::RankCacheMode::Off ? "true"
                                                                      : "false")
      << ",\n";
  out << "    \"rank_cache_mode\": ";
  write_json_string(out, rank_cache_mode_name(options.rank_cache_mode));
  out << ",\n";
  out << "    \"score_string_cache\": "
      << (options.score_string_cache ? "true" : "false") << ",\n";
  out << "    \"categories\": [";
  for (std::size_t i = 0; i < options.categories.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    write_json_string(out, options.categories[i]);
  }
  out << "],\n";
  out << "    \"metrics\": [";
  for (std::size_t i = 0; i < options.metrics.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    write_json_string(out, options.metrics[i]);
  }
  out << "]\n";
  out << "  },\n  \"results\": [\n";
  out << std::setprecision(17);
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    out << "    {\n";
    out << "      \"category\": ";
    write_json_string(out, result.category);
    out << ",\n      \"metric\": ";
    write_json_string(out, result.metric);
    out << ",\n";
    out << "      \"operations\": " << result.operations << ",\n";
    out << "      \"seconds\": " << result.seconds << ",\n";
    out << "      \"ns_per_op\": " << result.ns_per_op << ",\n";
    out << "      \"ops_per_second\": " << result.ops_per_second << ",\n";
    out << "      \"checksum\": " << result.checksum << "\n";
    out << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
}

void write_results(std::ostream& out,
                   const Options& options,
                   std::span<const BenchmarkResult> results) {
  if (options.format == "json") {
    write_json(out, options, results);
  } else if (options.format == "csv") {
    write_csv(out, results);
  } else {
    write_markdown(out, options, results);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_args(argc, argv, options)) {
    print_usage(std::cerr, argv[0]);
    return 2;
  }

  if (options.list_benchmarks) {
    list_benchmark_catalog(std::cout);
    return 0;
  }

  if (!options.categories.empty()) {
    std::cerr << "categories:";
    for (const auto& category : options.categories) {
      std::cerr << ' ' << category;
    }
    std::cerr << '\n';
  }
  if (!options.metrics.empty()) {
    std::cerr << "metrics:";
    for (const auto& metric : options.metrics) {
      std::cerr << ' ' << metric;
    }
    std::cerr << '\n';
  }

  std::cerr << "building fixture: members=" << options.members
            << " ops=" << options.ops
            << " range_size=" << options.range_size
            << " score_shape=" << score_shape_name(options.score_shape)
            << " rank_cache=" << rank_cache_mode_name(options.rank_cache_mode)
            << " score_string_cache="
            << (options.score_string_cache ? "on" : "off") << '\n';
  auto fixture = build_fixture(options);
  std::cerr << "running microbenchmarks\n";
  const auto results = run_benchmarks(options, fixture);
  if ((!options.categories.empty() || !options.metrics.empty()) &&
      results.empty()) {
    std::cerr << "no benchmarks matched the requested filters\n";
    return 2;
  }

  if (options.output.empty()) {
    write_results(std::cout, options, results);
    return 0;
  }

  std::ofstream file(options.output);
  if (!file) {
    std::cerr << "failed to open output file: " << options.output << '\n';
    return 1;
  }
  write_results(file, options, results);
  return 0;
}
