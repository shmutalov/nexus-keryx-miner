#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
namespace keryx::nexus::ai {
inline std::uint64_t sibling_saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  return b > UINT64_MAX-a ? UINT64_MAX : a+b;
}
// total_work_daa includes prompt processing AND generation, without audit padding.
// No waiting is permitted without a positive, current canonical awaiting check.
inline std::uint32_t sibling_wait_ms(std::uint64_t rank, std::uint64_t total_work_daa,
    std::uint64_t now_daa, std::uint64_t deadline, std::uint64_t elapsed_ns) noexcept {
  if (!rank || !deadline || now_daa >= deadline) return 0;
  const auto twice = total_work_daa > UINT64_MAX/2 ? UINT64_MAX : total_work_daa*2;
  if (sibling_saturating_add(sibling_saturating_add(now_daa, twice), 600) >= deadline) return 0;
  const auto per_rank = sibling_saturating_add(total_work_daa/10, 15);
  const auto delay = rank > UINT64_MAX/per_rank ? UINT64_MAX : rank*per_rank;
  const auto elapsed = elapsed_ns/1'000'000'000ULL;
  if (elapsed >= delay) return 0;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(delay-elapsed, 2)*1000);
}
}
