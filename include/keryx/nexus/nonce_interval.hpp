#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace keryx::nexus {
// All CUDA winners use UINT64_MAX as the no-winner sentinel. Reserve it on
// every backend so a terminal interval has identical semantics on both lanes.
inline std::uint64_t bounded_nonce_batch(std::uint64_t start,std::uint64_t last,
                                        std::uint64_t requested) {
  if(!requested || start>last || start==UINT64_MAX) return 0;
  if(last==UINT64_MAX) --last;
  return std::min(requested,last-start+1);
}
inline bool consume_nonce_interval(std::uint64_t &cursor,std::uint64_t last,std::uint64_t count) {
  if(!count || bounded_nonce_batch(cursor,last,count)!=count)
    throw std::invalid_argument("nonce interval exceeds reservation");
  if(last==UINT64_MAX) --last;
  const bool exhausted=count==last-cursor+1;
  if(!exhausted) cursor+=count;
  return exhausted;
}
inline std::uint64_t checked_winner_nonce(std::uint64_t start,std::uint64_t count,std::int64_t offset) {
  if(offset<0 || static_cast<std::uint64_t>(offset)>=count || count>UINT64_MAX-start)
    throw std::runtime_error("GPU winner outside reserved nonce interval");
  return start+static_cast<std::uint64_t>(offset);
}
}
