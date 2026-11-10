#pragma once
#include <cstdint>
#include <charconv>
#include <sstream>
#include <string>
#include <string_view>
#include "rpc.pb.h"
namespace keryx::nexus::ai {
inline constexpr std::uint64_t escrow_claim_check_seconds = 1800;
inline bool escrow_claim_check_due(std::uint64_t monotonic_now, std::uint64_t monotonic_next,
                                   std::uint64_t unix_now, std::uint64_t unix_last) noexcept {
  return monotonic_now >= monotonic_next &&
      (unix_last == 0 || (unix_now >= unix_last && unix_now - unix_last >= escrow_claim_check_seconds));
}
// Persisted claim receipts are only a scheduling hint: the next claim still
// needs the complete native ownership/UTXO/maturity/signature checks.
inline bool escrow_burn_receipt_backlog(std::string_view line) {
  if(line.find("\"state\":\"RPC_REJECTED\"")==std::string_view::npos)return false;
  constexpr std::string_view key="\"detail_hex\":\"";
  auto at=line.find(key);if(at==std::string_view::npos)return false;at+=key.size();
  const auto end=line.find('"',at);
  if(end==std::string_view::npos||end-at>32768||(end-at)%2)return false;
  std::string detail;
  for(;at<end;at+=2){
    const auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
    const int a=nibble(line[at]),b=nibble(line[at+1]);if(a<0||b<0)return false;
    detail.push_back(static_cast<char>(a*16+b));
  }
  return detail.find("burned escrow outpoints: ")!=std::string::npos;
}
inline bool escrow_claim_cycle_due(std::uint64_t now,std::uint64_t next,
    std::uint64_t unix_now,std::uint64_t last,bool burn_backlog) noexcept {
  return now>=next && (last==0 || (unix_now>=last && unix_now-last>=(burn_backlog?60:escrow_claim_check_seconds)));
}

// Only a correlated explicit rejection naming members of this exact claim
// authorizes immediate re-batching. UNKNOWN and generic errors never do.
inline bool escrow_named_burn_rejection(std::string_view detail,
                                        const protowire::RpcTransaction &tx) {
  constexpr std::string_view marker="burned escrow outpoints: ";
  const auto at=detail.rfind(marker);
  if(at==std::string_view::npos)return false;
  std::istringstream in(std::string(detail.substr(at+marker.size())));
  std::string token;unsigned named=0;
  while(in>>token){
    const auto colon=token.find(':');
    if(colon!=64)return false;
    const auto id=token.substr(0,colon);
    if(id.find_first_not_of("0123456789abcdef")!=std::string::npos)return false;
    unsigned index{};
    const auto begin=token.data()+colon+1,end=token.data()+token.size();
    const auto parsed=std::from_chars(begin,end,index);
    if(parsed.ec!=std::errc{}||parsed.ptr!=end||parsed.ptr==begin)return false;
    bool member=false;
    for(const auto &input:tx.inputs())
      if(input.previousoutpoint().transactionid()==id && input.previousoutpoint().index()==index)member=true;
    if(!member)return false;
    if(++named>static_cast<unsigned>(tx.inputs_size()))return false;
  }
  return named!=0;
}
inline constexpr unsigned escrow_burn_repair_attempts=8;

}
