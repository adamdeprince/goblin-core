#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core::numa {

enum class HardwareKind {
  NetworkInterface,
  InfiniBandDevice,
};

struct NodeInfo {
  int node{-1};
  std::string cpus;
};

struct HardwareTarget {
  HardwareKind kind{HardwareKind::NetworkInterface};
  std::string name;
  int node{-1};
};

struct Topology {
  std::vector<NodeInfo> nodes;
  std::vector<HardwareTarget> hardware;
};

struct EndpointPlacement {
  std::string source;
  std::string device;
  int node{-1};
};

enum class AutoNodeStatus {
  None,
  Resolved,
  Ambiguous,
  Conflict,
};

struct AutoNodeDecision {
  AutoNodeStatus status{AutoNodeStatus::None};
  int node{-1};
};

// Linux topology is read from sysfs. Other platforms return an empty inventory.
[[nodiscard]] Topology discover_topology();

// A selector may be a node number, "nodeN", a network interface, an InfiniBand
// device, or "auto". "auto" returns nullopt without setting an error.
[[nodiscard]] std::optional<int> resolve_target(
    std::string_view selector, const Topology& topology, std::string& error);

// Resolve a local bind address to its network interface(s). A wildcard address
// returns every physical network interface with a known NUMA node. Loopback is
// deliberately omitted because it is not a hardware placement constraint.
[[nodiscard]] std::vector<EndpointPlacement> resolve_local_address(
    std::string_view address, std::string_view source,
    const Topology& topology, std::string& error);

// Select automatically only when all hardware observations agree.
[[nodiscard]] AutoNodeDecision choose_auto_node(
    std::span<const EndpointPlacement> placements) noexcept;

[[nodiscard]] std::string format_topology(const Topology& topology);

}  // namespace goblin::core::numa
