#pragma once
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
namespace keryx::nexus::consensus {
struct CanonicalLayoutInspection {
  bool contiguous{};
  std::size_t first_mismatch{};
};
inline CanonicalLayoutInspection inspect_canonical_layout(
    std::span<const std::uint64_t> bases, std::span<const std::uint64_t> prefix) {
  if(bases.empty() || prefix.size()!=bases.size()+1 || prefix.front()!=0 || bases.front()%16)
    return {false,0};
  for(std::size_t i=0;i<bases.size();++i) {
    if(prefix[i+1]<=prefix[i] || prefix[i]>(std::numeric_limits<std::uint64_t>::max()-bases.front())/32 ||
        bases[i]!=bases.front()+prefix[i]*32) return {false,i};
  }
  return {true,bases.size()};
}
struct CanonicalTensorSlot {std::uint64_t address{},bytes{};bool device{};};
// Match the engine-owned canonical stream before filling its holes. Contract
// derived from Keryx-Labs/keryx-miner 4167e409 src/pom_gpu.rs; no allocation here.
inline bool canonical_blob_matches(std::span<const CanonicalTensorSlot> tensors,
                                  std::uint64_t base,std::uint64_t bytes) {
  if(!base || base%16 || !bytes || bytes>UINT64_MAX-base || tensors.empty())return false;
  std::uint64_t offset=0;
  for(const auto &t:tensors) {
    if(!t.address || t.bytes%32 || t.bytes>bytes-offset)return false;
    if(t.device) {
      if(t.bytes>UINT64_MAX-t.address)return false;
      if(t.address>=base && t.address<base+bytes) {
        if(t.address!=base+offset || t.bytes>base+bytes-t.address)return false;
      } else if(t.address<base && t.bytes>base-t.address)return false;
    }
    offset+=t.bytes;
  }
  return offset==bytes;
}
struct ResidentRoute {
  std::string_view kernel,reason;
  bool lockstep2{};
};
inline ResidentRoute resident_route(bool v5,bool contiguous,bool x2,bool trace) {
  if(!v5) return {"v4","era_v4",false};
  if(trace) return {"v5_fallback","trace",false};
  if(!contiguous) return {"v5_fallback","no_contiguous_buffer",false};
  if(!x2) return {"v5_fallback","no_function",false};
  return {"v5_lockstep2","contiguous_buffer",true};
}
}
