#include "goblin/core/numa_topology.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace goblin::core::numa {
namespace {

[[nodiscard]] std::optional<int> parse_nonnegative_int(
    std::string_view text) noexcept {
  int value = -1;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] int effective_node(int reported_node,
                                 const Topology& topology) noexcept {
  if (reported_node >= 0) {
    return reported_node;
  }
  return topology.nodes.size() == 1 ? topology.nodes.front().node : -1;
}

#if defined(__linux__)

[[nodiscard]] std::optional<int> read_node(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  int node = -1;
  if (!(input >> node)) {
    return std::nullopt;
  }
  return node;
}

[[nodiscard]] std::string read_line(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string line;
  std::getline(input, line);
  return line;
}

void discover_hardware(const std::filesystem::path& directory,
                       HardwareKind kind, Topology& topology) {
  std::error_code error;
  std::filesystem::directory_iterator entries(directory, error);
  if (error) {
    return;
  }
  for (const auto& entry : entries) {
    const auto node = read_node(entry.path() / "device" / "numa_node");
    topology.hardware.push_back(HardwareTarget{
        .kind = kind,
        .name = entry.path().filename().string(),
        .node = node.value_or(-1),
    });
  }
}

[[nodiscard]] bool wildcard_address(std::string_view address) noexcept {
  return address.empty() || address == "0.0.0.0" || address == "::" ||
         address == "*";
}

[[nodiscard]] bool same_address(const sockaddr* left,
                                const sockaddr* right) noexcept {
  if (left == nullptr || right == nullptr || left->sa_family != right->sa_family) {
    return false;
  }
  if (left->sa_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(left);
    const auto* b = reinterpret_cast<const sockaddr_in*>(right);
    return a->sin_addr.s_addr == b->sin_addr.s_addr;
  }
  if (left->sa_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(left);
    const auto* b = reinterpret_cast<const sockaddr_in6*>(right);
    return std::equal(std::begin(a->sin6_addr.s6_addr),
                      std::end(a->sin6_addr.s6_addr),
                      std::begin(b->sin6_addr.s6_addr));
  }
  return false;
}

[[nodiscard]] const HardwareTarget* find_network_target(
    const Topology& topology, std::string_view name) noexcept {
  const auto found = std::find_if(
      topology.hardware.begin(), topology.hardware.end(),
      [&](const HardwareTarget& target) {
        return target.kind == HardwareKind::NetworkInterface &&
               target.name == name;
      });
  return found == topology.hardware.end() ? nullptr : &*found;
}

#endif

}  // namespace

Topology discover_topology() {
  Topology topology;
#if defined(__linux__)
  const std::filesystem::path nodes("/sys/devices/system/node");
  std::error_code error;
  std::filesystem::directory_iterator entries(nodes, error);
  if (!error) {
    for (const auto& entry : entries) {
      const std::string name = entry.path().filename().string();
      if (!name.starts_with("node")) {
        continue;
      }
      const auto node = parse_nonnegative_int(std::string_view(name).substr(4));
      if (node) {
        topology.nodes.push_back(NodeInfo{
            .node = *node,
            .cpus = read_line(entry.path() / "cpulist"),
        });
      }
    }
  }
  discover_hardware("/sys/class/net", HardwareKind::NetworkInterface,
                    topology);
  discover_hardware("/sys/class/infiniband", HardwareKind::InfiniBandDevice,
                    topology);
  std::ranges::sort(topology.nodes, {}, &NodeInfo::node);
  std::ranges::sort(topology.hardware, [](const HardwareTarget& left,
                                          const HardwareTarget& right) {
    if (left.node != right.node) return left.node < right.node;
    if (left.kind != right.kind) return left.kind < right.kind;
    return left.name < right.name;
  });
#endif
  return topology;
}

std::optional<int> resolve_target(std::string_view selector,
                                  const Topology& topology,
                                  std::string& error) {
  error.clear();
  if (selector == "auto") {
    return std::nullopt;
  }
  std::string_view number = selector;
  if (selector.starts_with("node")) {
    number.remove_prefix(4);
  }
  if (const auto parsed = parse_nonnegative_int(number)) {
    const bool present = std::ranges::any_of(
        topology.nodes, [&](const NodeInfo& node) { return node.node == *parsed; });
    if (!present) {
      error = "NUMA node " + std::to_string(*parsed) + " is not present";
      return std::nullopt;
    }
    return parsed;
  }

  std::optional<int> selected;
  bool found = false;
  for (const auto& target : topology.hardware) {
    if (target.name != selector) {
      continue;
    }
    found = true;
    const int node = effective_node(target.node, topology);
    if (node < 0) {
      continue;
    }
    if (selected && *selected != node) {
      error = "hardware target '" + std::string(selector) +
              "' spans more than one NUMA node";
      return std::nullopt;
    }
    selected = node;
  }
  if (selected) {
    return selected;
  }
  error = found ? "hardware target '" + std::string(selector) +
                      "' has no NUMA node"
                : "unknown NUMA target '" + std::string(selector) + "'";
  return std::nullopt;
}

std::vector<EndpointPlacement> resolve_local_address(
    std::string_view address, std::string_view source,
    const Topology& topology, std::string& error) {
  std::vector<EndpointPlacement> result;
  error.clear();
#if defined(__linux__)
  if (wildcard_address(address)) {
    for (const auto& target : topology.hardware) {
      const int node = effective_node(target.node, topology);
      if (target.kind == HardwareKind::NetworkInterface && target.name != "lo") {
        result.push_back(EndpointPlacement{
            .source = std::string(source),
            .device = target.name,
            .node = node,
        });
      }
    }
    if (result.empty()) {
      error = "wildcard bind address has no NUMA-resolvable network interface";
    }
    return result;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* resolved = nullptr;
  const std::string address_text(address);
  const int lookup = ::getaddrinfo(address_text.c_str(), nullptr, &hints, &resolved);
  if (lookup != 0) {
    error = "cannot resolve local address '" + address_text + "': " +
            gai_strerror(lookup);
    return result;
  }

  ifaddrs* interfaces = nullptr;
  if (::getifaddrs(&interfaces) != 0) {
    ::freeaddrinfo(resolved);
    error = "cannot enumerate local network interfaces";
    return result;
  }

  std::set<std::string> matched_names;
  bool loopback_only = false;
  for (const ifaddrs* interface = interfaces; interface != nullptr;
       interface = interface->ifa_next) {
    if (interface->ifa_addr == nullptr || interface->ifa_name == nullptr) {
      continue;
    }
    bool matched = false;
    for (const addrinfo* candidate = resolved; candidate != nullptr;
         candidate = candidate->ai_next) {
      if (same_address(interface->ifa_addr, candidate->ai_addr)) {
        matched = true;
        break;
      }
    }
    if (!matched || !matched_names.emplace(interface->ifa_name).second) {
      continue;
    }
    if (std::string_view(interface->ifa_name) == "lo") {
      loopback_only = true;
      continue;
    }
    const HardwareTarget* target =
        find_network_target(topology, interface->ifa_name);
    if (target == nullptr) {
      result.push_back(EndpointPlacement{
          .source = std::string(source),
          .device = interface->ifa_name,
          .node = -1,
      });
    } else {
      result.push_back(EndpointPlacement{
          .source = std::string(source),
          .device = target->name,
          .node = effective_node(target->node, topology),
      });
    }
  }
  ::freeifaddrs(interfaces);
  ::freeaddrinfo(resolved);

  if (result.empty() && !loopback_only) {
    error = "address '" + address_text +
            "' does not identify a local NUMA-resolvable network interface";
  }
#else
  (void)address;
  (void)source;
  (void)topology;
#endif
  return result;
}

AutoNodeDecision choose_auto_node(
    std::span<const EndpointPlacement> placements) noexcept {
  AutoNodeDecision decision;
  bool unknown = false;
  for (const auto& placement : placements) {
    if (placement.node < 0) {
      unknown = true;
      continue;
    }
    if (decision.status == AutoNodeStatus::None) {
      decision.status = AutoNodeStatus::Resolved;
      decision.node = placement.node;
    } else if (decision.node != placement.node) {
      decision.status = AutoNodeStatus::Conflict;
      decision.node = -1;
      return decision;
    }
  }
  if (unknown) {
    decision.status = AutoNodeStatus::Ambiguous;
    decision.node = -1;
  }
  return decision;
}

std::string format_topology(const Topology& topology) {
  std::ostringstream output;
  output << "Available NUMA slices and hardware targets:\n";
  if (topology.nodes.empty()) {
    output << "  (no NUMA topology discovered)\n";
    return output.str();
  }
  for (const auto& node : topology.nodes) {
    output << "  node " << node.node << " CPUs "
           << (node.cpus.empty() ? "unknown" : node.cpus) << '\n';
    for (const auto& target : topology.hardware) {
      if (target.node != node.node) {
        continue;
      }
      output << "    "
             << (target.kind == HardwareKind::NetworkInterface ? "NIC "
                                                                : "InfiniBand ")
             << target.name << '\n';
    }
  }
  for (const auto& target : topology.hardware) {
    if (target.node < 0 && target.name != "lo") {
      output << "  unknown node: "
             << (target.kind == HardwareKind::NetworkInterface ? "NIC "
                                                                : "InfiniBand ")
             << target.name << '\n';
    }
  }
  output << "Choose with --numa NODE|NIC|INFINIBAND_DEVICE or --cpu CPU.\n";
  return output.str();
}

}  // namespace goblin::core::numa
