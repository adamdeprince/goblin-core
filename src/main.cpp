#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/server.hpp"
#include "goblin/core/simd.hpp"
#include "goblin/core/store.hpp"

#include <bit>
#include <charconv>
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
  if (ec != std::errc{} || ptr != end || !(value > 1.0)) {
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

void print_usage(std::string_view program) {
  std::cerr << "usage: " << program
            << " [--bind ADDRESS] [--port PORT] [--unixsocket PATH]\n"
            << "       [--rank-cache|--no-rank-cache]\n"
            << "       [--rank-cache-mode off|exact|block-hint]\n"
            << "       [--score-string-cache|--no-score-string-cache]\n"
            << "       [--member-index-growth FACTOR] [--load-factor N]\n"
            << "       [--block-shrink on|off]\n"
            << "       [--zset-chunk-bytes BYTES] [--hash-chunk-bytes BYTES]\n"
            << "       [--load SNAPSHOT]\n"
            << "       [--max-output-buffer-mib MIB]\n"
            << "       [--initial-output-buffer-kib KIB]\n"
            << "       [--client-read-buffer-kib KIB]\n"
            << "       [--ring PATH SIZE]...  (e.g. --ring /tmp/a 4kb; repeatable)\n";
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
            << " avx512vl=" << caps.avx512vl << '\n';

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
