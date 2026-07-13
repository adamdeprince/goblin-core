#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/server.hpp"
#include "goblin/core/simd.hpp"
#include "goblin/core/store.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::optional<std::uint16_t> parse_port(std::string_view text) {
  int value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || value <= 0 || value > 65535) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::optional<std::size_t> parse_mib(std::string_view text) {
  unsigned long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  constexpr unsigned long long bytes_per_mib = 1024ULL * 1024ULL;
  if (ec != std::errc{} || ptr != end ||
      value > std::numeric_limits<std::size_t>::max() / bytes_per_mib) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value * bytes_per_mib);
}

[[nodiscard]] std::optional<std::size_t> parse_kib(std::string_view text) {
  unsigned long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  constexpr unsigned long long bytes_per_kib = 1024ULL;
  if (ec != std::errc{} || ptr != end ||
      value > std::numeric_limits<std::size_t>::max() / bytes_per_kib) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value * bytes_per_kib);
}

[[nodiscard]] std::optional<std::size_t> parse_positive_size(
    std::string_view text) {
  unsigned long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || value == 0 ||
      value > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] std::optional<std::size_t> parse_nonnegative_size(
    std::string_view text) {
  unsigned long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end ||
      value > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] std::optional<std::size_t> parse_chunk_bytes(
    std::string_view text, std::size_t min_bytes) {
  unsigned long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end ||
      value > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  const auto bytes = static_cast<std::size_t>(value);
  // The arena addresses a chunk with a shift/mask, so the chunk size must be a
  // power of two, and large enough to hold the biggest item.
  if (!std::has_single_bit(bytes) || bytes < min_bytes) {
    return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] std::optional<double> parse_growth(std::string_view text) {
  double value = 0.0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || !(value > 1.0) ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] std::optional<double> parse_density(std::string_view text) {
  double value = 0.0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || !(value > 0.0) || value > 1.0 ||
      !std::isfinite(value)) {
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

[[nodiscard]] std::optional<goblin::core::ListImplementation>
parse_list_implementation(std::string_view text) {
  if (text == "pma") {
    return goblin::core::ListImplementation::Pma;
  }
  if (text == "segmented") {
    return goblin::core::ListImplementation::Segmented;
  }
  return std::nullopt;
}

void print_usage(std::string_view program) {
  std::cerr << "usage: " << program
            << " [--bind ADDRESS] [--port PORT] [--unixsocket PATH]\n"
            << "       [--rank-cache|--no-rank-cache]\n"
            << "       [--rank-cache-mode off|exact|block-hint]\n"
            << "       [--score-string-cache|--no-score-string-cache]\n"
            << "       [--member-index-growth FACTOR] [--load-factor N]\n"
            << "       [--block-shrink on|off]\n"
            << "       [--zset-chunk-bytes BYTES] [--hash-chunk-bytes BYTES]\n"
            << "       [--hash-compaction-knapsack|--no-hash-compaction-knapsack]\n"
            << "       [--hash-compaction-work-budget N]\n"
            << "       [--hash-listpack-max-entries N|blob]\n"
            << "       [--list-implementation pma|segmented]"
               " (default: segmented)\n"
            << "       [--list-chunk-bytes BYTES]\n"
            << "       [--list-max-density FRACTION]\n"
            << "       [--list-resize-growth FACTOR]\n"
            << "       [--disable-encoding]\n"
            << "       [--use-lz4 BYTES] [--lz4-compress-level LEVEL]\n"
            << "       [--load SNAPSHOT]\n"
            << "       [--max-output-buffer-mib MIB]\n"
            << "       [--initial-output-buffer-kib KIB]\n"
            << "       [--client-read-buffer-kib KIB]\n"
            << "       [--ring PATH SIZE]...  (e.g. --ring /tmp/a 4kb; repeatable)\n"
            << "       [--ring-hugetlb]       (Linux: back rings with huge pages)\n"
            << "       [--arena-hugetlb]      (back arena blocks with huge pages;\n"
            << "                               unsafe with fork-COW SAVE, so off by default)\n"
            << "       [--cpu N]              (Linux: pin to CPU N; ring must be NUMA-local)\n"
            << "       [--numa-arena]         (Linux: also prefer that node for arenas)\n";
}

}  // namespace

int main(int argc, char** argv) {
  goblin::core::ServerConfig config;
  goblin::core::StoreOptions store_options;
  std::optional<std::string> load_path;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }

    if (arg == "--bind") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      config.bind_address = argv[++i];
      continue;
    }

    if (arg == "--port") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto port = parse_port(argv[++i]);
      if (!port) {
        std::cerr << "goblin-core: invalid port\n";
        return 2;
      }
      config.port = *port;
      continue;
    }

    if (arg == "--unixsocket") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      config.unix_socket_path = argv[++i];
      continue;
    }

    if (arg == "--ring") {
      // Two tokens: a file path and a size (bytes, or with a kb/mb/gb suffix).
      // Repeatable; ring order is priority order (first is highest, lowest latency).
      if (i + 2 >= argc) {
        std::cerr << "goblin-core: --ring requires PATH and SIZE\n";
        return 2;
      }
      const std::string path = argv[++i];
      const std::string_view size_text = argv[++i];
      const auto size = goblin::core::ring::parse_size(size_text);
      if (!size || *size == 0) {
        std::cerr << "goblin-core: invalid --ring size: " << size_text << '\n';
        return 2;
      }
      config.rings.push_back(
          goblin::core::RingConfig{.path = path, .bytes = *size});
      continue;
    }

    if (arg == "--ring-hugetlb") {
      // Back every ring with huge pages. Linux-only -- macOS has no hugetlb.
#if defined(__linux__)
      config.ring_hugetlb = true;
#else
      std::cerr << "goblin-core: --ring-hugetlb is only supported on Linux\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--arena-hugetlb") {
      // Back max-size arena blocks with huge pages (OFF by default). Opt-in, not the
      // default, because SAVE's fork+COW is unsafe with huge pages: 2 MiB COW
      // granularity blows up RSS and SIGBUSes the parent if the pool exhausts mid-save.
      // A no-op where huge pages are unavailable, so it is accepted on every platform.
      goblin::core::hugetlb::arena_enabled() = true;
      continue;
    }

    if (arg == "--cpu") {
      // Pin the server to this CPU and place the ring on its NUMA node (fatal if not
      // local). Linux-only; a no-op elsewhere.
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view text = argv[++i];
      int cpu = 0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), cpu);
      if (ec != std::errc{} || ptr != text.data() + text.size() || cpu < 0) {
        std::cerr << "goblin-core: invalid --cpu value: " << text << '\n';
        return 2;
      }
      config.cpu = cpu;
      continue;
    }

    if (arg == "--numa-arena") {
      // Also steer arena allocations to the pinned CPU's node (soft). Needs --cpu.
      config.numa_arena = true;
      continue;
    }

    if (arg == "--rank-cache") {
      store_options.rank_cache_mode = goblin::core::RankCacheMode::Exact;
      continue;
    }

    if (arg == "--no-rank-cache") {
      store_options.rank_cache_mode = goblin::core::RankCacheMode::Off;
      continue;
    }

    if (arg == "--rank-cache-mode") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto mode = parse_rank_cache_mode(argv[++i]);
      if (!mode) {
        std::cerr << "goblin-core: invalid rank cache mode\n";
        return 2;
      }
      store_options.rank_cache_mode = *mode;
      continue;
    }

    if (arg == "--member-index-growth") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto growth = parse_growth(argv[++i]);
      if (!growth) {
        std::cerr << "goblin-core: --member-index-growth must be > 1\n";
        return 2;
      }
      store_options.member_index_growth = *growth;
      continue;
    }

    if (arg == "--load-factor") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view text(argv[++i]);
      std::size_t load = 0;
      const auto [ptr, ec] =
          std::from_chars(text.data(), text.data() + text.size(), load);
      if (ec != std::errc{} || ptr != text.data() + text.size() || load < 1) {
        std::cerr << "goblin-core: --load-factor must be a positive integer\n";
        return 2;
      }
      store_options.zset_score_index_load = load;
      continue;
    }

    if (arg == "--block-shrink") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view v(argv[++i]);
      if (v == "on" || v == "1" || v == "true") {
        goblin::core::ZSetScoreIndex::trim_enabled_ = true;
      } else if (v == "off" || v == "0" || v == "false") {
        goblin::core::ZSetScoreIndex::trim_enabled_ = false;
      } else {
        std::cerr << "goblin-core: --block-shrink must be on or off\n";
        return 2;
      }
      continue;
    }

    if (arg == "--zset-chunk-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_chunk_bytes(
          argv[++i], goblin::core::ZSetMemberStorage::kMinChunkBytes);
      if (!bytes) {
        std::cerr << "goblin-core: --zset-chunk-bytes must be a power of two >= "
                  << goblin::core::ZSetMemberStorage::kMinChunkBytes
                  << " bytes\n";
        return 2;
      }
      store_options.zset_chunk_bytes = *bytes;
      continue;
    }

    if (arg == "--hash-chunk-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_chunk_bytes(
          argv[++i], goblin::core::HashStorage::kMinChunkBytes);
      if (!bytes) {
        std::cerr << "goblin-core: --hash-chunk-bytes must be a power of two >= "
                  << goblin::core::HashStorage::kMinChunkBytes << " bytes\n";
        return 2;
      }
      store_options.hash_chunk_bytes = *bytes;
      continue;
    }

    if (arg == "--hash-compaction-knapsack") {
      store_options.hash_compaction_knapsack = true;
      continue;
    }

    if (arg == "--no-hash-compaction-knapsack") {
      store_options.hash_compaction_knapsack = false;
      continue;
    }

    if (arg == "--hash-compaction-work-budget") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto budget = parse_positive_size(argv[++i]);
      constexpr std::size_t kMaxCompactionWorkBudget = std::size_t{1} << 20;
      if (!budget || *budget > kMaxCompactionWorkBudget) {
        std::cerr << "goblin-core: --hash-compaction-work-budget must be in "
                     "[1, 1048576]\n";
        return 2;
      }
      store_options.hash_compaction_work_budget = *budget;
      continue;
    }

    if (arg == "--hash-listpack-max-entries") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view text(argv[++i]);
      if (text == "blob") {
        store_options.hash_listpack_max_entries =
            goblin::core::HashOptions::kDefaultListpackMaxEntries;
        continue;
      }
      std::size_t entries = 0;
      const auto [ptr, ec] =
          std::from_chars(text.data(), text.data() + text.size(), entries);
      if (ec != std::errc{} || ptr != text.data() + text.size()) {
        std::cerr << "goblin-core: --hash-listpack-max-entries must be a "
                     "non-negative integer or blob\n";
        return 2;
      }
      store_options.hash_listpack_max_entries = entries;
      continue;
    }

    if (arg == "--list-chunk-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_chunk_bytes(
          argv[++i], goblin::core::ListValueArena::kMinChunkBytes);
      if (!bytes ||
          *bytes > goblin::core::ListValueArena::kMaxChunkBytes) {
        std::cerr << "goblin-core: --list-chunk-bytes must be a power of two >= "
                  << goblin::core::ListValueArena::kMinChunkBytes << " and <= "
                  << goblin::core::ListValueArena::kMaxChunkBytes << " bytes\n";
        return 2;
      }
      store_options.list_chunk_bytes = *bytes;
      continue;
    }

    if (arg == "--list-implementation") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto implementation = parse_list_implementation(argv[++i]);
      if (!implementation) {
        std::cerr << "goblin-core: --list-implementation must be pma or "
                     "segmented\n";
        return 2;
      }
      store_options.list_implementation = *implementation;
      continue;
    }

    if (arg == "--list-max-density") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto density = parse_density(argv[++i]);
      if (!density) {
        std::cerr << "goblin-core: --list-max-density must be in (0, 1]\n";
        return 2;
      }
      store_options.list_max_density = *density;
      continue;
    }

    if (arg == "--list-resize-growth") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto growth = parse_growth(argv[++i]);
      if (!growth) {
        std::cerr << "goblin-core: --list-resize-growth must be > 1\n";
        return 2;
      }
      store_options.list_resize_growth = *growth;
      continue;
    }

    if (arg == "--disable-encoding") {
      store_options.string_encoding.disable();
      continue;
    }

    if (arg == "--use-lz4") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto threshold = parse_nonnegative_size(argv[++i]);
      if (!threshold) {
        std::cerr << "goblin-core: --use-lz4 must be a non-negative integer\n";
        return 2;
      }
      store_options.string_encoding.set_lz4_min_bytes(*threshold);
      continue;
    }

    if (arg == "--lz4-compress-level") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view text(argv[++i]);
      int level = 0;
      const auto [ptr, ec] =
          std::from_chars(text.data(), text.data() + text.size(), level);
      if (ec != std::errc{} || ptr != text.data() + text.size() ||
          !goblin::core::StringEncodingOptions::valid_lz4_compress_level(
              level)) {
        std::cerr << "goblin-core: --lz4-compress-level must be 0, 3..12, "
                     "or -1..-8\n";
        return 2;
      }
      store_options.string_encoding.set_lz4_compress_level(level);
      continue;
    }

    if (arg == "--load") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      load_path = argv[++i];
      continue;
    }

    if (arg == "--score-string-cache") {
      store_options.score_string_cache = true;
      continue;
    }

    if (arg == "--no-score-string-cache") {
      store_options.score_string_cache = false;
      continue;
    }

    if (arg == "--max-output-buffer-mib") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_mib(argv[++i]);
      if (!bytes) {
        std::cerr << "goblin-core: invalid max output buffer MiB\n";
        return 2;
      }
      config.max_output_buffer_bytes = *bytes;
      config.resume_output_buffer_bytes = *bytes / 4;
      continue;
    }

    if (arg == "--initial-output-buffer-kib") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_kib(argv[++i]);
      if (!bytes) {
        std::cerr << "goblin-core: invalid initial output buffer KiB\n";
        return 2;
      }
      config.initial_output_buffer_bytes = *bytes;
      continue;
    }

    if (arg == "--client-read-buffer-kib") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_kib(argv[++i]);
      if (!bytes || *bytes == 0) {
        std::cerr << "goblin-core: invalid client read buffer KiB\n";
        return 2;
      }
      config.client_read_buffer_bytes = *bytes;
      continue;
    }

    std::cerr << "goblin-core: unknown option: " << arg << '\n';
    print_usage(argv[0]);
    return 2;
  }

  const auto caps = goblin::core::simd::detect_capabilities();
  std::cout << "goblin-core SIMD caps:"
            << " neon=" << caps.neon
            << " avx2=" << caps.avx2
            << " avx512bw=" << caps.avx512bw
            << " avx512vl=" << caps.avx512vl
            << " lsx=" << caps.lsx
            << " lasx=" << caps.lasx << '\n';

#if defined(__linux__)
  // NUMA: pin to the requested CPU and (opt-in) steer arena memory to its node, before
  // the store allocates so its arenas honor the policy. The ring's stricter, fatal-if-
  // remote binding happens at ring-creation time in the server.
  if (config.cpu >= 0) {
    if (!goblin::core::numa::pin_to_cpu(config.cpu)) {
      std::cerr << "goblin-core: --cpu " << config.cpu << ": failed to pin\n";
      return 1;
    }
    if (config.numa_arena) {
      const int node = goblin::core::numa::node_of_cpu(config.cpu);
      if (node < 0 || !goblin::core::numa::prefer_node(node)) {
        std::cerr << "goblin-core: --numa-arena: could not set a NUMA preference for "
                     "CPU " << config.cpu << " (continuing without it)\n";
      }
    }
  }
#endif

  goblin::core::Store store(store_options);

  if (load_path) {
    std::ifstream snapshot(*load_path, std::ios::binary);
    if (!snapshot) {
      std::cerr << "goblin-core: cannot open snapshot: " << *load_path << '\n';
      return 1;
    }
    try {
      const auto stats = store.load(snapshot);
      std::cout << "goblin-core: loaded " << stats.members << " members across "
                << stats.keys << " keys from " << *load_path
                << (stats.used_accelerator ? " (accelerated)" : " (rebuilt)")
                << '\n';
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: failed to load snapshot: " << error.what() << '\n';
      return 1;
    }
  }

  goblin::core::Server server(config, store);
  return server.run();
}
