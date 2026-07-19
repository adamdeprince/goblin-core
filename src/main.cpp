#include "goblin/core/numa.hpp"
#include "goblin/core/numa_topology.hpp"
#include "goblin/core/ring_buffer.hpp"
#include "goblin/core/server.hpp"
#include "goblin/core/simd.hpp"
#include "goblin/core/store.hpp"

#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

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

[[nodiscard]] std::string format_tcp_endpoint(std::string_view address,
                                              std::uint16_t port) {
  std::string result;
  if (address.find(':') != std::string_view::npos) {
    result.reserve(address.size() + 8);
    result.push_back('[');
    result.append(address);
    result.push_back(']');
  } else {
    result.assign(address);
  }
  result.push_back(':');
  result.append(std::to_string(port));
  return result;
}

[[nodiscard]] std::optional<goblin::core::TcpListenerConfig>
parse_tcp_listener(std::string_view endpoint, std::string& error) {
  std::string_view address;
  std::string_view port_text;
  if (endpoint.starts_with('[')) {
    const auto close = endpoint.find(']');
    if (close == std::string_view::npos || close == 1 ||
        close + 1 >= endpoint.size() || endpoint[close + 1] != ':') {
      error = "expected bracketed IPv6 endpoint [ADDRESS]:PORT";
      return std::nullopt;
    }
    address = endpoint.substr(1, close - 1);
    port_text = endpoint.substr(close + 2);
  } else {
    const auto separator = endpoint.rfind(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == endpoint.size()) {
      error = "expected ADDRESS:PORT";
      return std::nullopt;
    }
    if (endpoint.find(':') != separator) {
      error = "IPv6 addresses must use [ADDRESS]:PORT";
      return std::nullopt;
    }
    address = endpoint.substr(0, separator);
    port_text = endpoint.substr(separator + 1);
  }

  const auto port = parse_port(port_text);
  if (!port) {
    error = "port must be an integer from 1 through 65535";
    return std::nullopt;
  }
  return goblin::core::TcpListenerConfig{
      .bind_address = std::string(address), .port = *port};
}

[[nodiscard]] bool add_tcp_listener(
    goblin::core::ServerConfig& config,
    goblin::core::TcpListenerConfig listener) {
  for (const auto& configured : config.socket_listeners) {
    const auto* tcp = std::get_if<goblin::core::TcpListenerConfig>(&configured);
    if (tcp != nullptr && tcp->bind_address == listener.bind_address &&
        tcp->port == listener.port) {
      std::cerr << "goblin-core: duplicate --tcp-listen "
                << format_tcp_endpoint(listener.bind_address, listener.port)
                << '\n';
      return false;
    }
  }
  config.socket_listeners.emplace_back(std::move(listener));
  return true;
}

[[nodiscard]] bool add_uds_listener(goblin::core::ServerConfig& config,
                                    std::string path,
                                    std::string_view option) {
  if (path.empty()) {
    std::cerr << "goblin-core: " << option << " path must not be empty\n";
    return false;
  }
  for (const auto& configured : config.socket_listeners) {
    const auto* uds = std::get_if<goblin::core::UdsListenerConfig>(&configured);
    if (uds != nullptr && uds->path == path) {
      std::cerr << "goblin-core: duplicate UDS listener path: " << path << '\n';
      return false;
    }
  }
  config.socket_listeners.emplace_back(
      goblin::core::UdsListenerConfig{.path = std::move(path)});
  return true;
}

void materialize_legacy_listener(goblin::core::ServerConfig& config) {
  if (!config.socket_listeners.empty()) {
    return;
  }
  if (!config.unix_socket_path.empty()) {
    config.socket_listeners.emplace_back(
        goblin::core::UdsListenerConfig{.path = config.unix_socket_path});
  } else {
    config.socket_listeners.emplace_back(goblin::core::TcpListenerConfig{
        .bind_address = config.bind_address, .port = config.port});
  }
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

[[nodiscard]] std::optional<std::uint64_t> parse_nonnegative_seconds(
    std::string_view text) {
  std::uint64_t value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end ||
      value > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max() / 1000)) {
    return std::nullopt;
  }
  return value;
}

struct FileTimestamp {
  std::int64_t milliseconds{0};
  bool creation_time{false};
};

[[nodiscard]] std::optional<FileTimestamp> snapshot_file_timestamp(
    const std::string& path, std::string& error) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    error = std::strerror(errno);
    return std::nullopt;
  }

  std::int64_t seconds = 0;
  long nanoseconds = 0;
  bool creation_time = false;
#if defined(__APPLE__)
  seconds = status.st_birthtimespec.tv_sec;
  nanoseconds = status.st_birthtimespec.tv_nsec;
  creation_time = true;
#elif defined(__FreeBSD__)
  seconds = status.st_birthtim.tv_sec;
  nanoseconds = status.st_birthtim.tv_nsec;
  creation_time = true;
#elif defined(__linux__) && defined(SYS_statx) && defined(STATX_BTIME)
  struct statx extended {};
  if (::syscall(SYS_statx, AT_FDCWD, path.c_str(), AT_STATX_SYNC_AS_STAT,
                STATX_BTIME, &extended) == 0 &&
      (extended.stx_mask & STATX_BTIME) != 0) {
    seconds = extended.stx_btime.tv_sec;
    nanoseconds = static_cast<long>(extended.stx_btime.tv_nsec);
    creation_time = true;
  } else {
    seconds = status.st_mtim.tv_sec;
    nanoseconds = status.st_mtim.tv_nsec;
  }
#else
  seconds = status.st_mtime;
#endif

  if (seconds < 0 ||
      seconds > std::numeric_limits<std::int64_t>::max() / 1000) {
    error = "file timestamp is outside the supported range";
    return std::nullopt;
  }
  return FileTimestamp{
      .milliseconds = seconds * 1000 + nanoseconds / 1'000'000,
      .creation_time = creation_time};
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

[[nodiscard]] std::optional<goblin::core::HashImplementation>
parse_hash_implementation(std::string_view text) {
  if (text == "efficient") {
    return goblin::core::HashImplementation::Efficient;
  }
  if (text == "rt" || text == "real-time") {
    return goblin::core::HashImplementation::Realtime;
  }
  return std::nullopt;
}

void lock_server_memory() noexcept {
  if (::mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
    return;
  }
  const int error = errno;
  std::cerr << "goblin-core: warning: mlockall failed: "
            << std::strerror(error);
  if (error == ENOSYS) {
    std::cerr << "; core mmap regions remain individually locked, but the "
                 "general heap may be swapped on this OS\n";
  } else {
    std::cerr << "; memory may be swapped (raise RLIMIT_MEMLOCK or grant the "
                 "process permission to lock memory)\n";
  }
}

[[nodiscard]] bool configure_numa(
    goblin::core::ServerConfig& config,
    const std::optional<std::string>& selector) {
#if defined(__linux__)
  using goblin::core::numa::AutoNodeStatus;
  using goblin::core::numa::EndpointPlacement;

  const auto topology = goblin::core::numa::discover_topology();
  std::vector<EndpointPlacement> placements;
  const auto collect_address = [&](std::string_view address,
                                   std::string source) -> bool {
    std::string error;
    auto resolved = goblin::core::numa::resolve_local_address(
        address, source, topology, error);
    if (!error.empty()) {
      std::cerr << "goblin-core: NUMA discovery for " << source << ": "
                << error << '\n'
                << goblin::core::numa::format_topology(topology);
      return false;
    }
    placements.insert(placements.end(),
                      std::make_move_iterator(resolved.begin()),
                      std::make_move_iterator(resolved.end()));
    return true;
  };

  for (const auto& listener : config.socket_listeners) {
    if (const auto* tcp =
            std::get_if<goblin::core::TcpListenerConfig>(&listener);
        tcp != nullptr &&
        !collect_address(
            tcp->bind_address,
            "socket bind " +
                format_tcp_endpoint(tcp->bind_address, tcp->port))) {
      return false;
    }
  }
  for (const auto& target : config.poll_targets) {
    if (const auto* rdma = std::get_if<goblin::core::RdmaConfig>(&target)) {
      if (!collect_address(rdma->bind_address,
                           "RDMA bind " + rdma->bind_address + ':' +
                               std::to_string(rdma->port))) {
        return false;
      }
    } else if (const auto* exa =
                   std::get_if<goblin::core::ExasockConfig>(&target)) {
      if (!collect_address(exa->bind_address,
                           "ExaSock bind " + exa->bind_address + ':' +
                               std::to_string(exa->port))) {
        return false;
      }
    }
  }

  std::optional<int> selected;
  bool explicit_selection = false;
  if (selector && *selector != "auto") {
    std::string error;
    selected = goblin::core::numa::resolve_target(*selector, topology, error);
    if (!selected) {
      std::cerr << "goblin-core: --numa " << *selector << ": " << error << '\n'
                << goblin::core::numa::format_topology(topology);
      return false;
    }
    explicit_selection = true;
  }

  if (config.cpu >= 0) {
    const int cpu_node = goblin::core::numa::node_of_cpu(config.cpu);
    if (cpu_node < 0) {
      std::cerr << "goblin-core: --cpu " << config.cpu
                << " does not belong to a discovered NUMA node\n"
                << goblin::core::numa::format_topology(topology);
      return false;
    }
    if (selected && *selected != cpu_node) {
      std::cerr << "goblin-core: --cpu " << config.cpu << " is on NUMA node "
                << cpu_node << ", but --numa selects node " << *selected << '\n'
                << goblin::core::numa::format_topology(topology);
      return false;
    }
    selected = cpu_node;
    explicit_selection = true;
  }

  const auto automatic = goblin::core::numa::choose_auto_node(placements);
  if (!explicit_selection &&
      (automatic.status == AutoNodeStatus::Conflict ||
       automatic.status == AutoNodeStatus::Ambiguous)) {
    if (automatic.status == AutoNodeStatus::Conflict) {
      std::cerr << "goblin-core: configured network and InfiniBand targets span "
                   "different NUMA nodes; choose the slice explicitly\n";
    } else {
      std::cerr << "goblin-core: at least one configured network target has "
                   "unknown NUMA locality; choose the slice explicitly\n";
    }
    for (const auto& placement : placements) {
      std::cerr << "  " << placement.source << " via " << placement.device
                << " -> NUMA node "
                << (placement.node < 0 ? std::string("unknown")
                                       : std::to_string(placement.node))
                << '\n';
    }
    std::cerr << goblin::core::numa::format_topology(topology);
    return false;
  }
  if (!selected && automatic.status == AutoNodeStatus::Resolved) {
    selected = automatic.node;
  }

  config.numa_node = selected.value_or(-1);
  if (config.numa_node >= 0) {
    if (config.cpu >= 0) {
      if (!goblin::core::numa::pin_to_cpu(config.cpu)) {
        std::cerr << "goblin-core: --cpu " << config.cpu << ": failed to pin\n";
        return false;
      }
    } else if (!goblin::core::numa::pin_to_node(config.numa_node)) {
      std::cerr << "goblin-core: failed to restrict execution to NUMA node "
                << config.numa_node << '\n';
      return false;
    }

    std::cout << "goblin-core NUMA: node " << config.numa_node
              << (explicit_selection ? " selected explicitly" :
                                       " selected from transport hardware")
              << (config.cpu >= 0 ? ", CPU " + std::to_string(config.cpu) : "")
              << '\n';
    for (const auto& placement : placements) {
      if (explicit_selection && placement.node >= 0 &&
          placement.node != config.numa_node) {
        std::cerr << "goblin-core: warning: " << placement.source << " via "
                  << placement.device << " is local to NUMA node "
                  << placement.node << "; explicit placement uses node "
                  << config.numa_node << '\n';
      }
    }
  }

  if (config.numa_arena) {
    if (config.numa_node < 0 ||
        !goblin::core::numa::prefer_node(config.numa_node)) {
      std::cerr << "goblin-core: --numa-arena: could not set a NUMA preference"
                   " (continuing without it)\n";
    }
  }
  return true;
#else
  (void)config;
  if (selector && *selector != "auto") {
    std::cerr << "goblin-core: --numa is only supported on Linux\n";
    return false;
  }
  return true;
#endif
}

void print_usage(std::string_view program) {
  std::cerr << "usage: " << program
            << " [--tcp-listen ADDRESS:PORT]... [--uds-listen PATH]...\n"
            << "       [--bind ADDRESS] [--port PORT] [--unixsocket PATH]...\n"
            << "       [--rank-cache|--no-rank-cache]\n"
            << "       [--rank-cache-mode off|exact|block-hint]\n"
            << "       [--score-string-cache|--no-score-string-cache]\n"
            << "       [--member-index-growth FACTOR] [--load-factor N]\n"
            << "       [--block-shrink on|off]\n"
            << "       [--zset-chunk-bytes BYTES] [--hash-chunk-bytes BYTES]\n"
            << "       [--hash-compaction-knapsack|--no-hash-compaction-knapsack]\n"
            << "       [--hash-compaction-work-budget N]\n"
            << "       [--hash-listpack-max-entries N|blob]\n"
            << "       [--hash-implementation efficient|rt]\n"
            << "       [--rt-hash-index-bytes BYTES] (fixed prefaulted pool)\n"
            << "       [--real-time]         (RT hashes and keyspace index)\n"
            << "       [--list-implementation pma|segmented]"
               " (default: segmented)\n"
            << "       [--list-chunk-bytes BYTES]\n"
            << "       [--array-slice-slots N]   (power of two; leaf width ≤ 65536)\n"
            << "       [--array-initial-depth N] (Classic: start depth; RT: hard depth)\n"
            << "       [--array-chunk-bytes BYTES] (element arena; power of two)\n"
            << "       [--rt-array-arena-growth FACTOR] (default: 2.0)\n"
            << "       [--realtime-arrays]       (unqualified AR* → RT; default Classic)\n"
            << "       [--list-max-density FRACTION]\n"
            << "       [--list-resize-growth FACTOR]\n"
            << "       [--disable-encoding]\n"
            << "       [--use-lz4 BYTES] [--lz4-compress-level LEVEL]\n"
            << "       [--load SNAPSHOT]\n"
            << "       [--kafka CONNECTION] [--kafka-time-buffer SECONDS]\n"
            << "       [--auth-file FILE]\n"
            << "       [--enable-sbe] [--no-auth-ring] [--no-auth-rdma]\n"
            << "       [--max-output-buffer-mib MIB]\n"
            << "       [--initial-output-buffer-kib KIB]\n"
            << "       [--unsolicited-output-buffer-bytes BYTES]\n"
            << "       [--transaction-buffer-bytes BYTES]\n"
            << "       [--client-read-buffer-kib KIB]\n"
            << "       [--ring PATH SIZE]...  (e.g. --ring /tmp/a 4kb; repeatable)\n"
            << "       [--exasock ADDRESS PORT]..."
               " (priority TCP; -DGOBLIN_CORE_ENABLE_EXASOCK=ON;\n"
            << "                               run under `exasock` for SmartNIC bypass)\n"
            << "       [--rdma ADDRESS PORT SIZE]..."
               " (e.g. --rdma 10.88.88.1 6380 1mb; repeatable;\n"
            << "                               requires -DGOBLIN_CORE_ENABLE_RDMA=ON)\n"
            << "       [--pubsub-listener-ring PATH]\n"
            << "       [--pubsub-listener-rdma ADDRESS PORT SIZE]\n"
            << "       [--pubsub-listener-uds PATH]\n"
            << "       [--pubsub-listener-tcp HOST PORT]\n"
            << "       [--pubsub-listener-pattern GLOB] (default: *)\n"
            << "       [--ring-hugetlb]       (Linux: back rings with huge pages)\n"
            << "       [--arena-hugetlb]      (back arena blocks with huge pages;\n"
            << "                               unsafe with fork-COW SAVE, so off by default)\n"
            << "       [--cpu N]              (Linux: pin to CPU N; ring must be NUMA-local)\n"
            << "       [--numa TARGET]        (Linux: node, NIC, InfiniBand device, or auto)\n"
            << "       [--numa-arena]         (Linux: also prefer that node for arenas)\n"
            << "\n"
            << "Polled-target order is literal CLI order and is the busy-poll priority\n"
            << "before plain sockets. Example:\n"
            << "  --ring /tmp/a 64kb --exasock 10.99.99.1 6379 --rdma 10.88.88.1 6380 1mb\n"
            << "  --ring /tmp/b 1mb\n"
            << "scans A, ExaSock, RDMA, B, then the sparse plain-socket pass.\n"
            << "\n"
            << "Socket listeners are repeatable and may be mixed. IPv6 uses\n"
            << "--tcp-listen [ADDRESS]:PORT. Legacy --bind/--port selects one TCP\n"
            << "listener only when no explicit socket listener is configured; legacy\n"
            << "--unixsocket is a repeatable alias for --uds-listen.\n";
  std::cerr
      << "Kafka CONNECTION is kafka://BROKER[,BROKER...]/TOPIC or\n"
      << "bootstrap.servers=BROKERS;topic=TOPIC[;PROPERTY=VALUE...].\n"
      << "--auth-file protects RESP. SBE requires --enable-sbe and never\n"
      << "authenticates; --no-auth-ring/--no-auth-rdma explicitly trust RESP\n"
      << "on those fabrics.\n";
}

}  // namespace

int main(int argc, char** argv) {
  goblin::core::ServerConfig config;
  goblin::core::StoreOptions store_options;
  std::optional<std::string> load_path;
  std::optional<std::string> kafka_connection;
  std::uint64_t kafka_time_buffer_seconds = 0;
  bool kafka_time_buffer_set = false;
  std::optional<std::string> numa_selector;
  bool pubsub_listener_pattern_set = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }

    if (arg == "--auth-file") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --auth-file requires a path\n";
        return 2;
      }
      if (config.auth_file) {
        std::cerr << "goblin-core: configure only one --auth-file\n";
        return 2;
      }
      config.auth_file = argv[++i];
      continue;
    }

    if (arg == "--enable-sbe") {
      config.enable_sbe = true;
      continue;
    }

    if (arg == "--no-auth-ring") {
      config.no_auth_ring = true;
      continue;
    }

    if (arg == "--no-auth-rdma") {
      config.no_auth_rdma = true;
      continue;
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

    if (arg == "--tcp-listen") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --tcp-listen requires ADDRESS:PORT\n";
        return 2;
      }
      const std::string_view endpoint = argv[++i];
      std::string error;
      auto listener = parse_tcp_listener(endpoint, error);
      if (!listener) {
        std::cerr << "goblin-core: invalid --tcp-listen '" << endpoint
                  << "': " << error << '\n';
        return 2;
      }
      if (!add_tcp_listener(config, std::move(*listener))) {
        return 2;
      }
      continue;
    }

    if (arg == "--uds-listen") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --uds-listen requires PATH\n";
        return 2;
      }
      if (!add_uds_listener(config, argv[++i], "--uds-listen")) {
        return 2;
      }
      continue;
    }

    if (arg == "--unixsocket") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      std::string path = argv[++i];
      config.unix_socket_path = path;
      if (!add_uds_listener(config, std::move(path), "--unixsocket")) {
        return 2;
      }
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
      config.poll_targets.emplace_back(
          goblin::core::RingConfig{.path = path, .bytes = *size});
      continue;
    }

    if (arg == "--exasock") {
#if defined(GOBLIN_HAS_EXASOCK)
      // Two tokens: bind address and port. Repeatable; interleaved with --ring
      // and --rdma so CLI order is busy-poll priority order.
      if (i + 2 >= argc) {
        std::cerr << "goblin-core: --exasock requires ADDRESS and PORT\n";
        return 2;
      }
      const std::string address = argv[++i];
      const std::string_view port_text = argv[++i];
      const auto port = parse_port(port_text);
      if (!port) {
        std::cerr << "goblin-core: invalid --exasock port: " << port_text
                  << '\n';
        return 2;
      }
      config.poll_targets.emplace_back(goblin::core::ExasockConfig{
          .bind_address = address, .port = *port});
#else
      std::cerr << "goblin-core: --exasock is unavailable in this build"
                   " (configure with -DGOBLIN_CORE_ENABLE_EXASOCK=ON and an"
                   " installed ExaSock SDK)\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--rdma") {
#if defined(GOBLIN_HAS_RDMA)
      // Three tokens: the IPoIB/RoCE bind address, CM port, and registered-ring
      // capacity. Repeatable and freely interleavable with --ring / --exasock;
      // that literal option order is the strict busy-poll priority order.
      if (i + 3 >= argc) {
        std::cerr << "goblin-core: --rdma requires ADDRESS, PORT, and SIZE\n";
        return 2;
      }
      const std::string address = argv[++i];
      const std::string_view port_text = argv[++i];
      const std::string_view size_text = argv[++i];
      const auto port = parse_port(port_text);
      const auto size = goblin::core::ring::parse_size(size_text);
      if (!port) {
        std::cerr << "goblin-core: invalid --rdma port: " << port_text << '\n';
        return 2;
      }
      if (!size || *size == 0) {
        std::cerr << "goblin-core: invalid --rdma size: " << size_text << '\n';
        return 2;
      }
      config.poll_targets.emplace_back(goblin::core::RdmaConfig{
          .bind_address = address, .port = *port, .bytes = *size});
#else
      std::cerr << "goblin-core: --rdma is unavailable in this build"
                   " (requires Linux, libibverbs, and librdmacm;"
                   " -DGOBLIN_CORE_ENABLE_RDMA=ON)\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--pubsub-listener-ring") {
#if defined(GOBLIN_HAS_SBE)
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --pubsub-listener-ring requires PATH\n";
        return 2;
      }
      if (config.pubsub_listener) {
        std::cerr << "goblin-core: configure only one Pub/Sub listener\n";
        return 2;
      }
      config.pubsub_listener = goblin::core::PubSubListenerRingConfig{
          .path = argv[++i]};
#else
      std::cerr << "goblin-core: --pubsub-listener-ring requires SBE support\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--pubsub-listener-rdma") {
#if defined(GOBLIN_HAS_SBE) && defined(GOBLIN_HAS_RDMA)
      if (i + 3 >= argc) {
        std::cerr << "goblin-core: --pubsub-listener-rdma requires ADDRESS, "
                     "PORT, and SIZE\n";
        return 2;
      }
      if (config.pubsub_listener) {
        std::cerr << "goblin-core: configure only one Pub/Sub listener\n";
        return 2;
      }
      const std::string address = argv[++i];
      const std::string_view port_text = argv[++i];
      const std::string_view size_text = argv[++i];
      const auto port = parse_port(port_text);
      const auto size = goblin::core::ring::parse_size(size_text);
      if (!port) {
        std::cerr << "goblin-core: invalid --pubsub-listener-rdma port: "
                  << port_text << '\n';
        return 2;
      }
      if (!size || *size == 0) {
        std::cerr << "goblin-core: invalid --pubsub-listener-rdma size: "
                  << size_text << '\n';
        return 2;
      }
      config.pubsub_listener = goblin::core::PubSubListenerRdmaConfig{
          .address = address, .port = *port, .bytes = *size};
#else
      std::cerr << "goblin-core: --pubsub-listener-rdma is unavailable in this "
                   "build (requires SBE, Linux, libibverbs, and librdmacm)\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--pubsub-listener-uds") {
#if defined(GOBLIN_HAS_SBE)
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --pubsub-listener-uds requires PATH\n";
        return 2;
      }
      if (config.pubsub_listener) {
        std::cerr << "goblin-core: configure only one Pub/Sub listener\n";
        return 2;
      }
      config.pubsub_listener =
          goblin::core::PubSubListenerUdsConfig{.path = argv[++i]};
#else
      std::cerr << "goblin-core: --pubsub-listener-uds requires SBE support\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--pubsub-listener-tcp") {
#if defined(GOBLIN_HAS_SBE)
      if (i + 2 >= argc) {
        std::cerr << "goblin-core: --pubsub-listener-tcp requires HOST and PORT\n";
        return 2;
      }
      if (config.pubsub_listener) {
        std::cerr << "goblin-core: configure only one Pub/Sub listener\n";
        return 2;
      }
      const std::string address = argv[++i];
      const std::string_view port_text = argv[++i];
      const auto port = parse_port(port_text);
      if (!port) {
        std::cerr << "goblin-core: invalid --pubsub-listener-tcp port: "
                  << port_text << '\n';
        return 2;
      }
      config.pubsub_listener = goblin::core::PubSubListenerTcpConfig{
          .address = address, .port = *port};
#else
      std::cerr << "goblin-core: --pubsub-listener-tcp requires SBE support\n";
      return 2;
#endif
      continue;
    }

    if (arg == "--pubsub-listener-pattern") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --pubsub-listener-pattern requires GLOB\n";
        return 2;
      }
      config.pubsub_listener_pattern = argv[++i];
      pubsub_listener_pattern_set = true;
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

    if (arg == "--numa") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      numa_selector = argv[++i];
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

    if (arg == "--hash-implementation") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto implementation = parse_hash_implementation(argv[++i]);
      if (!implementation) {
        std::cerr << "goblin-core: --hash-implementation must be efficient "
                     "or rt\n";
        return 2;
      }
      store_options.hash_implementation = *implementation;
      continue;
    }

    if (arg == "--rt-hash-index-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const std::string_view text(argv[++i]);
      const auto bytes = goblin::core::ring::parse_size(text);
      if (!bytes || *bytes == 0 ||
          *bytes > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "goblin-core: invalid RT hash index arena size\n";
        return 2;
      }
      store_options.realtime_hash_index_bytes =
          static_cast<std::size_t>(*bytes);
      continue;
    }

    if (arg == "--real-time") {
      store_options.real_time = true;
      store_options.hash_implementation =
          goblin::core::HashImplementation::Realtime;
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

    if (arg == "--array-slice-slots") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto slots = parse_nonnegative_size(argv[++i]);
      if (!slots || *slots < 2 || (*slots & (*slots - 1)) != 0) {
        std::cerr
            << "goblin-core: --array-slice-slots must be a power of two >= 2\n";
        return 2;
      }
      store_options.array_slice_slots = *slots;
      continue;
    }

    if (arg == "--array-initial-depth") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto depth = parse_nonnegative_size(argv[++i]);
      if (!depth || *depth < 1 || *depth > 16) {
        std::cerr
            << "goblin-core: --array-initial-depth must be in [1, 16]\n";
        return 2;
      }
      store_options.array_initial_depth = *depth;
      continue;
    }

    if (arg == "--array-chunk-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_chunk_bytes(
          argv[++i], goblin::core::ArrayStorage::kMinChunkBytes);
      if (!bytes) {
        std::cerr
            << "goblin-core: --array-chunk-bytes must be a power of two >= "
            << goblin::core::ArrayStorage::kMinChunkBytes << " bytes\n";
        return 2;
      }
      store_options.array_chunk_bytes = *bytes;
      continue;
    }

    if (arg == "--rt-array-arena-growth") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto growth = parse_growth(argv[++i]);
      if (!growth) {
        std::cerr << "goblin-core: --rt-array-arena-growth must be > 1\n";
        return 2;
      }
      store_options.realtime_array_growth = *growth;
      continue;
    }

    if (arg == "--realtime-arrays") {
      store_options.array_implementation =
          goblin::core::ArrayImplementation::Realtime;
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

    if (arg == "--kafka") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --kafka requires a connection string\n";
        return 2;
      }
      if (kafka_connection) {
        std::cerr << "goblin-core: configure only one --kafka source\n";
        return 2;
      }
      kafka_connection = argv[++i];
      continue;
    }

    if (arg == "--kafka-time-buffer") {
      if (i + 1 >= argc) {
        std::cerr << "goblin-core: --kafka-time-buffer requires seconds\n";
        return 2;
      }
      const auto seconds = parse_nonnegative_seconds(argv[++i]);
      if (!seconds) {
        std::cerr << "goblin-core: --kafka-time-buffer must be a non-negative "
                     "integer number of seconds\n";
        return 2;
      }
      kafka_time_buffer_seconds = *seconds;
      kafka_time_buffer_set = true;
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

    if (arg == "--unsolicited-output-buffer-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_positive_size(argv[++i]);
      if (!bytes) {
        std::cerr << "goblin-core: invalid unsolicited output buffer byte count\n";
        return 2;
      }
      config.unsolicited_output_buffer_bytes = *bytes;
      continue;
    }

    if (arg == "--transaction-buffer-bytes") {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 2;
      }
      const auto bytes = parse_positive_size(argv[++i]);
      if (!bytes) {
        std::cerr << "goblin-core: invalid transaction buffer byte count\n";
        return 2;
      }
      config.transaction_buffer_bytes = *bytes;
      continue;
    }

    std::cerr << "goblin-core: unknown option: " << arg << '\n';
    print_usage(argv[0]);
    return 2;
  }

  materialize_legacy_listener(config);

  if (kafka_time_buffer_set && !kafka_connection) {
    std::cerr << "goblin-core: --kafka-time-buffer requires --kafka\n";
    return 2;
  }
  if (kafka_time_buffer_set && !load_path) {
    std::cerr << "goblin-core: --kafka-time-buffer requires --load; without a "
                 "snapshot Kafka starts at the earliest retained record\n";
    return 2;
  }
#ifndef GOBLIN_HAS_KAFKA
  if (kafka_connection) {
    std::cerr << "goblin-core: --kafka requires a build with "
                 "-DGOBLIN_CORE_ENABLE_KAFKA=ON\n";
    return 2;
  }
#endif

  if (pubsub_listener_pattern_set && !config.pubsub_listener) {
    std::cerr << "goblin-core: --pubsub-listener-pattern requires "
                 "a --pubsub-listener-* transport\n";
    return 2;
  }
  if (const auto* listener =
          config.pubsub_listener
              ? std::get_if<goblin::core::PubSubListenerRingConfig>(
                    &*config.pubsub_listener)
              : nullptr) {
    for (const auto& target : config.poll_targets) {
      if (const auto* ring = std::get_if<goblin::core::RingConfig>(&target);
          ring != nullptr && ring->path == listener->path) {
        std::cerr << "goblin-core: Pub/Sub listener ring must not be one of this "
                     "server's --ring paths\n";
        return 2;
      }
    }
  }
  if (const auto* listener =
          config.pubsub_listener
              ? std::get_if<goblin::core::PubSubListenerUdsConfig>(
                    &*config.pubsub_listener)
              : nullptr) {
    for (const auto& configured : config.socket_listeners) {
      const auto* uds =
          std::get_if<goblin::core::UdsListenerConfig>(&configured);
      if (uds != nullptr && uds->path == listener->path) {
        std::cerr << "goblin-core: Pub/Sub listener UDS path must not be one "
                     "of this server's --uds-listen/--unixsocket paths\n";
        return 2;
      }
    }
  }
  if (const auto* listener =
          config.pubsub_listener
              ? std::get_if<goblin::core::PubSubListenerTcpConfig>(
                    &*config.pubsub_listener)
              : nullptr) {
    for (const auto& configured : config.socket_listeners) {
      const auto* tcp =
          std::get_if<goblin::core::TcpListenerConfig>(&configured);
      if (tcp != nullptr && tcp->bind_address == listener->address &&
          tcp->port == listener->port) {
        std::cerr << "goblin-core: Pub/Sub listener TCP endpoint must not be "
                     "one of this server's --tcp-listen endpoints\n";
        return 2;
      }
    }
  }

  const auto caps = goblin::core::simd::detect_capabilities();
  std::cout << "goblin-core SIMD caps:"
            << " neon=" << caps.neon
            << " avx2=" << caps.avx2
            << " avx512bw=" << caps.avx512bw
            << " avx512vl=" << caps.avx512vl
            << " lsx=" << caps.lsx
            << " lasx=" << caps.lasx << '\n';

  // Resolve transport locality before the Store allocates. Explicit --numa/--cpu
  // wins; otherwise every configured hardware endpoint must agree on one slice.
  if (!configure_numa(config, numa_selector)) {
    return 1;
  }

  // Keep the server heap and every future allocation resident. The call can be
  // restricted by RLIMIT_MEMLOCK in ordinary shells and containers, so failure
  // is visible but does not make the compatibility server unusable there.
  lock_server_memory();

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

  if (kafka_connection) {
    std::optional<std::int64_t> start_timestamp_ms;
    if (load_path) {
      std::string timestamp_error;
      const auto timestamp =
          snapshot_file_timestamp(*load_path, timestamp_error);
      if (!timestamp) {
        std::cerr << "goblin-core: cannot determine snapshot timestamp: "
                  << timestamp_error << '\n';
        return 1;
      }
      if (!timestamp->creation_time) {
        std::cerr << "goblin-core: snapshot creation time is unavailable; "
                     "using modification time for Kafka replay\n";
      }
      const auto buffer_ms = static_cast<std::int64_t>(
          kafka_time_buffer_seconds * 1000);
      start_timestamp_ms =
          timestamp->milliseconds > buffer_ms
              ? timestamp->milliseconds - buffer_ms
              : 0;
    }
    config.kafka = goblin::core::KafkaConfig{
        .connection = std::move(*kafka_connection),
        .start_timestamp_ms = start_timestamp_ms};
  }

  goblin::core::Server server(config, store);
  return server.run();
}
