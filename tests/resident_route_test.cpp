#include "keryx/nexus/consensus/resident_route.hpp"
#include <array>
#include "keryx/nexus/nonce_interval.hpp"
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::consensus;
void check(bool b){if(!b)throw std::runtime_error("resident route invariant failed");}
int main(){
  for(unsigned bits:{8U,16U,32U,64U}) {
    const auto last=bits==64?UINT64_MAX:((std::uint64_t(1)<<bits)-1);
    std::uint64_t cursor=last-7;
    const auto count=keryx::nexus::bounded_nonce_batch(cursor,last,1024);
    check(count==(bits==64?7:8));
    check(keryx::nexus::consume_nonce_interval(cursor,last,count));
    check(keryx::nexus::checked_winner_nonce(last-7,count,0)==last-7);
    bool rejected=false;try{keryx::nexus::checked_winner_nonce(last-7,count,count);}catch(const std::exception&){rejected=true;}
    check(rejected);
  }
  check(keryx::nexus::bounded_nonce_batch(UINT64_MAX,UINT64_MAX,1)==0);
  std::uint64_t cursor=4;check(!keryx::nexus::consume_nonce_interval(cursor,255,5)&&cursor==9);

  const std::array<CanonicalTensorSlot,3> slots{{{4096,64,true},{9000,32,false},{4192,64,true}}};
  check(canonical_blob_matches(slots,4096,160));
  auto wrong=slots;wrong[2].address=4096;check(!canonical_blob_matches(wrong,4096,160));
  check(!canonical_blob_matches(slots,4096,192));
  check(!canonical_blob_matches(slots,UINT64_MAX-15,160));
  wrong=slots;wrong[2].address=4080;check(!canonical_blob_matches(wrong,4096,160));
  wrong=slots;wrong[2].address=9000;check(canonical_blob_matches(wrong,4096,160));
  const std::array<std::uint64_t,2> contiguous{4096,4192},segmented{4096,8192};
  const std::array<std::uint64_t,3> prefix{0,3,8},unsorted{0,8,3};
  check(inspect_canonical_layout(contiguous,prefix).contiguous);
  const auto bad=inspect_canonical_layout(segmented,prefix);check(!bad.contiguous&&bad.first_mismatch==1);
  check(!inspect_canonical_layout(contiguous,unsorted).contiguous);
  check(!inspect_canonical_layout({},{}).contiguous);
  const std::array<std::uint64_t,3> overflow{0,UINT64_MAX-1,UINT64_MAX};
  check(!inspect_canonical_layout(contiguous,overflow).contiguous);
  for(bool v5:{false,true})for(bool layout:{false,true})for(bool function:{false,true})for(bool trace:{false,true}) {
    const auto route=resident_route(v5,layout,function,trace);
    check(route.lockstep2==(v5&&layout&&function&&!trace));
    if(v5&&!layout&&!trace)check(route.reason=="no_contiguous_buffer");
  }
  std::cout<<"RESIDENT_ROUTE=PASS combinations=16 layout_checks=5 GPU=NOT_RUN\n";
}
