#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
namespace keryx::nexus {
// Host resource policy only: does not change consensus pad size or proof bytes.
struct PomVramBudget {
  std::uint64_t pad_bytes{}, metadata_bytes{}, free_bytes{};
  static constexpr std::uint64_t reserve_bytes=128ULL*1024*1024;
  bool fits() const noexcept {
    return free_bytes>=reserve_bytes && metadata_bytes<=free_bytes-reserve_bytes &&
      pad_bytes<=free_bytes-reserve_bytes-metadata_bytes;
  }
};
class PomVramAdmissionDenied final: public std::runtime_error {
 public:
  PomVramBudget budget;
  bool allocation_race;
  explicit PomVramAdmissionDenied(PomVramBudget b,bool race=false)
    :std::runtime_error("POM_VRAM_ADMISSION_DENIED"),budget(b),allocation_race(race){}
};
inline void require_pom_vram(PomVramBudget b){if(!b.fits())throw PomVramAdmissionDenied(b);}
// Called outside GPU ownership. Failed admissions do not block AI/RPC polling.
class PomAdmissionBackoff {
 public:
  using Clock=std::chrono::steady_clock;
  bool due(Clock::time_point now)const noexcept{std::lock_guard lock(mutex_);return now>=next_;}
  bool degraded()const noexcept{std::lock_guard lock(mutex_);return failures_!=0;}
  std::uint64_t deny(Clock::time_point now){
    std::lock_guard lock(mutex_);failures_=std::min(failures_+1,6U);
    const auto ms=std::min<std::uint64_t>(1000ULL<<(failures_-1),30000);
    next_=now+std::chrono::milliseconds(ms);return ms;
  }
  bool recovered() noexcept{std::lock_guard lock(mutex_);const bool changed=failures_!=0;failures_=0;next_={};return changed;}
 private:mutable std::mutex mutex_;unsigned failures_{};Clock::time_point next_{};
};
}
