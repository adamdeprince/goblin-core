#include "goblin/core/numa_topology.hpp"

#include <array>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "numa_topology_test: " << message << '\n';
  }
  return condition;
}

}  // namespace

int main() {
  using namespace goblin::core::numa;

  const Topology topology{
      .nodes = {{.node = 0, .cpus = "0-7"},
                {.node = 1, .cpus = "8-15"}},
      .hardware = {
          {.kind = HardwareKind::NetworkInterface,
           .name = "eth0",
           .node = 0},
          {.kind = HardwareKind::NetworkInterface,
           .name = "ibp1s0",
           .node = 1},
          {.kind = HardwareKind::InfiniBandDevice,
           .name = "mlx5_0",
           .node = 1},
      },
  };

  std::string error;
  const auto numeric = resolve_target("1", topology, error);
  if (!expect(numeric && *numeric == 1 && error.empty(),
              "numeric node selector failed")) {
    return 1;
  }
  const auto prefixed = resolve_target("node0", topology, error);
  if (!expect(prefixed && *prefixed == 0 && error.empty(),
              "node-prefixed selector failed")) {
    return 1;
  }
  const auto nic = resolve_target("eth0", topology, error);
  if (!expect(nic && *nic == 0 && error.empty(), "NIC selector failed")) {
    return 1;
  }
  const auto infiniband = resolve_target("mlx5_0", topology, error);
  if (!expect(infiniband && *infiniband == 1 && error.empty(),
              "InfiniBand selector failed")) {
    return 1;
  }
  const auto invalid = resolve_target("eth9", topology, error);
  if (!expect(!invalid && !error.empty(), "unknown selector was accepted")) {
    return 1;
  }

  const Topology single_node{
      .nodes = {{.node = 0, .cpus = "0-3"}},
      .hardware = {{.kind = HardwareKind::NetworkInterface,
                    .name = "eth-unknown",
                    .node = -1}},
  };
  const auto single_node_nic =
      resolve_target("eth-unknown", single_node, error);
  if (!expect(single_node_nic && *single_node_nic == 0 && error.empty(),
              "single-node fallback rejected a valid NIC")) {
    return 1;
  }

  const std::array same{
      EndpointPlacement{.source = "TCP", .device = "eth0", .node = 0},
      EndpointPlacement{.source = "RDMA", .device = "mlx5_1", .node = 0},
  };
  const auto same_decision = choose_auto_node(same);
  if (!expect(same_decision.status == AutoNodeStatus::Resolved &&
                  same_decision.node == 0,
              "matching hardware nodes did not resolve")) {
    return 1;
  }

  const std::array conflict{
      EndpointPlacement{.source = "TCP", .device = "eth0", .node = 0},
      EndpointPlacement{.source = "RDMA", .device = "mlx5_0", .node = 1},
  };
  const auto conflict_decision = choose_auto_node(conflict);
  if (!expect(conflict_decision.status == AutoNodeStatus::Conflict,
              "different NIC and InfiniBand nodes did not conflict")) {
    return 1;
  }

  const std::array unknown{
      EndpointPlacement{.source = "virtual", .device = "bond0", .node = -1},
  };
  const auto unknown_decision = choose_auto_node(unknown);
  if (!expect(unknown_decision.status == AutoNodeStatus::Ambiguous,
              "unknown locality did not require an explicit choice")) {
    return 1;
  }
  return 0;
}
