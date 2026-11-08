#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace keryx::nexus::platform {

enum class CpuRole {
  PrimaryNvidia, // Primary CUDA worker (and Rust Oracle)
  SecondaryAmd,  // Secondary Vulkan worker
  NodeSystem,    // Node daemon & desktop system
  Solo           // Dedicated single GPU miner
};

inline const char *cpu_role_name(CpuRole role) noexcept {
  switch (role) {
  case CpuRole::PrimaryNvidia:
    return "PRIMARY_NVIDIA";
  case CpuRole::SecondaryAmd:
    return "SECONDARY_AMD";
  case CpuRole::NodeSystem:
    return "NODE_SYSTEM";
  case CpuRole::Solo:
    return "SOLO";
  }
  return "UNKNOWN";
}

struct CpuPartition {
  std::vector<int> cpu_ids;
  unsigned int recommended_proof_threads{2};

  std::string to_string() const {
    if (cpu_ids.empty())
      return "ALL";
    std::ostringstream ss;
    for (std::size_t i = 0; i < cpu_ids.size(); ++i) {
      if (i > 0)
        ss << ",";
      ss << cpu_ids[i];
    }
    return ss.str();
  }
};

class HostCpuTopology {
public:
  static unsigned int total_threads() noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
      unsigned int count = 0;
      for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &set))
          ++count;
      }
      if (count > 0)
        return count;
    }
#endif
    const unsigned int hw = std::thread::hardware_concurrency();
    return (hw > 0) ? hw : 4;
  }

  // Maps physical core ID -> list of logical CPU thread IDs (for SMT / HyperThreading)
  static std::map<int, std::vector<int>> discover_cores() {
    std::map<int, std::vector<int>> core_map;
    const unsigned int total = total_threads();

#if defined(__linux__)
    namespace fs = std::filesystem;
    for (unsigned int cpu = 0; cpu < total; ++cpu) {
      const fs::path p = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                         "/topology/core_id";
      if (fs::exists(p)) {
        std::ifstream f(p);
        int core_id = -1;
        if (f >> core_id && core_id >= 0) {
          core_map[core_id].push_back(static_cast<int>(cpu));
          continue;
        }
      }
      // Fallback: pair consecutive CPUs if SMT is likely
      const int fallback_core = (total >= 4) ? static_cast<int>(cpu / 2) : static_cast<int>(cpu);
      core_map[fallback_core].push_back(static_cast<int>(cpu));
    }
#else
    for (unsigned int cpu = 0; cpu < total; ++cpu) {
      const int core = (total >= 4) ? static_cast<int>(cpu / 2) : static_cast<int>(cpu);
      core_map[core].push_back(static_cast<int>(cpu));
    }
#endif
    return core_map;
  }

  // Dynamic adaptive partitioner: adapts to ANY number of cores (from 2/4 up to 32+)
  static CpuPartition partition(CpuRole role) {
    if (const char *env_disable = std::getenv("NEXUS_CPU_AFFINITY_DISABLED")) {
      if (std::string_view(env_disable) == "1" ||
          std::string_view(env_disable) == "true") {
        return {}; // Empty means all CPUs unconstrained
      }
    }

    // Explicit user override via env var
    const char *custom = nullptr;
    if (role == CpuRole::PrimaryNvidia)
      custom = std::getenv("NEXUS_CPU_AFFINITY_PRIMARY");
    else if (role == CpuRole::SecondaryAmd)
      custom = std::getenv("NEXUS_CPU_AFFINITY_SECONDARY");

    if (custom && *custom) {
      CpuPartition custom_part;
      std::stringstream ss(custom);
      std::string token;
      while (std::getline(ss, token, ',')) {
        try {
          custom_part.cpu_ids.push_back(std::stoi(token));
        } catch (...) {}
      }
      if (!custom_part.cpu_ids.empty()) {
        custom_part.recommended_proof_threads = std::max(
            1u, std::min(8u, static_cast<unsigned int>(custom_part.cpu_ids.size())));
        return custom_part;
      }
    }

    const auto core_map = discover_cores();
    std::vector<std::vector<int>> cores;
    cores.reserve(core_map.size());
    for (const auto &[_, threads] : core_map) {
      cores.push_back(threads);
    }

    const std::size_t num_cores = cores.size();
    const unsigned int total = total_threads();
    CpuPartition result;

    // --- Scenario 1: Low Core Count (<= 4 hardware threads or <= 2 physical cores) ---
    // In constrained environments (e.g. 4-core laptops or 2-core / 4-thread VMs),
    // strict partitioning causes thread starvation. All workloads share all threads.
    if (total <= 4 || num_cores <= 2) {
      result.cpu_ids.clear(); // unconstrained
      result.recommended_proof_threads = std::max(1u, total / 2);
      return result;
    }

    // Helper to append all threads from a range of physical cores
    auto append_cores = [&](std::size_t start, std::size_t count) {
      for (std::size_t i = start; i < start + count && i < cores.size(); ++i) {
        for (int tid : cores[i]) {
          result.cpu_ids.push_back(tid);
        }
      }
    };

    // --- Scenario 2: Medium Core Count (5 to 8 hardware threads, e.g. 4 cores / 8 threads, 6 cores) ---
    if (total <= 8 || num_cores <= 4) {
      if (role == CpuRole::Solo) {
        // Dedicate 3/4 of cores to miner, 1 core to system
        const std::size_t miner_cores = std::max<std::size_t>(1, num_cores - 1);
        append_cores(0, miner_cores);
        result.recommended_proof_threads = std::max(1u, static_cast<unsigned>(result.cpu_ids.size() / 2));
      } else if (role == CpuRole::PrimaryNvidia) {
        // 50% of cores to Primary
        const std::size_t prim_cores = std::max<std::size_t>(1, num_cores / 2);
        append_cores(0, prim_cores);
        result.recommended_proof_threads = 2;
      } else if (role == CpuRole::SecondaryAmd) {
        // Next core to Secondary
        const std::size_t start = std::max<std::size_t>(1, num_cores / 2);
        append_cores(start, 1);
        result.recommended_proof_threads = 2;
      } else { // NodeSystem
        // Remaining core(s) to Node and System
        const std::size_t start = (num_cores > 2) ? (num_cores - 1) : 0;
        append_cores(start, num_cores - start);
        result.recommended_proof_threads = 1;
      }
      return result;
    }

    // --- Scenario 3: High Core Count (9 to 16 threads, e.g. 8 cores / 16 threads like Ryzen 7800X3D) ---
    if (total <= 16 || num_cores <= 8) {
      // Typically 8 physical cores:
      // Primary NVIDIA: 4 cores (cores 0-3, 8 threads)
      // Secondary AMD: 2 cores (cores 4-5, 4 threads)
      // Node/System: 2 cores (cores 6-7, 4 threads)
      const std::size_t prim_count = num_cores / 2;                        // 4 cores
      const std::size_t sec_count = std::max<std::size_t>(1, (num_cores - prim_count) / 2); // 2 cores
      const std::size_t node_start = prim_count + sec_count;

      if (role == CpuRole::Solo || role == CpuRole::PrimaryNvidia) {
        append_cores(0, prim_count);
        result.recommended_proof_threads = 4;
      } else if (role == CpuRole::SecondaryAmd) {
        append_cores(prim_count, sec_count);
        result.recommended_proof_threads = std::min(4u, static_cast<unsigned>(result.cpu_ids.size()));
      } else { // NodeSystem
        append_cores(node_start, num_cores - node_start);
        result.recommended_proof_threads = 2;
      }
      return result;
    }

    // --- Scenario 4: Extreme Core Count (> 16 threads, e.g. 12/16/32+ cores) ---
    // Reserve 2 cores (4-8 threads) for system/node, divide the rest 60% / 40%
    const std::size_t sys_cores = std::min<std::size_t>(4, std::max<std::size_t>(2, num_cores / 8));
    const std::size_t available_compute = num_cores - sys_cores;
    const std::size_t prim_cores = (available_compute * 6) / 10;
    const std::size_t sec_cores = available_compute - prim_cores;

    if (role == CpuRole::Solo) {
      append_cores(0, available_compute);
      result.recommended_proof_threads = 8;
    } else if (role == CpuRole::PrimaryNvidia) {
      append_cores(0, prim_cores);
      result.recommended_proof_threads = 8;
    } else if (role == CpuRole::SecondaryAmd) {
      append_cores(prim_cores, sec_cores);
      result.recommended_proof_threads = std::min(8u, static_cast<unsigned>(result.cpu_ids.size()));
    } else { // NodeSystem
      append_cores(prim_cores + sec_cores, sys_cores);
      result.recommended_proof_threads = 4;
    }
    return result;
  }

  // Apply the adaptive affinity on current calling process/thread
  static bool apply_affinity(CpuRole role) {
    const CpuPartition part = partition(role);
    const char *role_str = cpu_role_name(role);

    if (part.cpu_ids.empty()) {
      std::cout << "ADAPTIVE_CPU_AFFINITY role=" << role_str
                << " cpus=ALL_SHARED total_threads=" << total_threads()
                << "\n" << std::flush;
      return true;
    }

#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu : part.cpu_ids) {
      if (cpu >= 0 && cpu < CPU_SETSIZE) {
        CPU_SET(cpu, &set);
      }
    }
    if (sched_setaffinity(0, sizeof(set), &set) == 0) {
      std::cout << "ADAPTIVE_CPU_AFFINITY role=" << role_str
                << " cpus=" << part.to_string()
                << " proof_threads=" << part.recommended_proof_threads
                << " total_threads=" << total_threads()
                << "\n" << std::flush;
      return true;
    } else {
      std::cerr << "ADAPTIVE_CPU_AFFINITY_WARN: sched_setaffinity failed for "
                << role_str << "\n" << std::flush;
      return false;
    }
#else
    return true;
#endif
  }
};

inline bool apply_adaptive_cpu_affinity(CpuRole role) {
  return HostCpuTopology::apply_affinity(role);
}

inline CpuPartition get_adaptive_partition(CpuRole role) {
  return HostCpuTopology::partition(role);
}

} // namespace keryx::nexus::platform
