#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace keryx::nexus::stratum {
inline constexpr char nexus_pool_version[] = "0.0.5-dev";
inline constexpr char nexus_pool_agent[] = "nexus-keryx-miner/0.0.5-dev";
struct PoolGpuIdentity {
  unsigned index{};
  std::string vendor, model;
};
struct PoolTelemetryOptions {
  bool enabled{false};
  unsigned interval_seconds{120};
  std::string build{"native"};
  std::string model_id;
  unsigned tier{};
  // Pool trace readiness is distinct from a SOLO inference self-test.
  bool trace_ready{false};
  std::vector<PoolGpuIdentity> devices;
};
std::string pool_hello_json(const PoolTelemetryOptions &);
std::string pool_metrics_json(const PoolTelemetryOptions &, std::uint64_t uptime,
    std::optional<std::uint64_t> walks_per_second, std::uint64_t accepted,
    std::uint64_t rejected, std::uint64_t stale, bool busy);
} // namespace keryx::nexus::stratum
