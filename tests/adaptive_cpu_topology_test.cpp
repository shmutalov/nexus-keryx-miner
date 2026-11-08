#include "keryx/nexus/cpu_topology.hpp"
#include <cassert>
#include <iostream>

using namespace keryx::nexus::platform;

int main() {
  std::cout << "Testing HostCpuTopology...\n";
  const unsigned int total = HostCpuTopology::total_threads();
  std::cout << "Detected total hardware threads: " << total << "\n";
  assert(total > 0);

  const auto part_primary = HostCpuTopology::partition(CpuRole::PrimaryNvidia);
  const auto part_secondary = HostCpuTopology::partition(CpuRole::SecondaryAmd);
  const auto part_node = HostCpuTopology::partition(CpuRole::NodeSystem);

  std::cout << "Primary Partition:   " << part_primary.to_string() << "\n";
  std::cout << "Secondary Partition: " << part_secondary.to_string() << "\n";
  std::cout << "Node Partition:      " << part_node.to_string() << "\n";

  if (total >= 8) {
    // Check that primary and secondary don't collide if total >= 8
    assert(!part_primary.cpu_ids.empty());
    assert(!part_secondary.cpu_ids.empty());
    for (int p_cpu : part_primary.cpu_ids) {
      for (int s_cpu : part_secondary.cpu_ids) {
        assert(p_cpu != s_cpu);
      }
    }
  }

  // Test apply_affinity (should succeed on Linux)
  const bool ok = apply_adaptive_cpu_affinity(CpuRole::SecondaryAmd);
  assert(ok);

  std::cout << "HostCpuTopology PASS\n";
  return 0;
}
