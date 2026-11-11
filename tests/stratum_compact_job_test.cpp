#include "keryx/nexus/stratum_v4_client.hpp"
#include "keryx/nexus/json_integer_wire.hpp"
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace keryx::nexus;
using namespace keryx::nexus::stratum;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
 try {
  StratumClientConfig config; config.pool_url="stratum+tcp://127.0.0.1:1"; config.worker="synthetic";
  StratumV4Client client(config); client.enable_loopback_test_mode(true);
  check(client.connect_and_handshake_once(),"handshake fixture");
  auto frames=client.take_outbound_wire_log();check(frames.size()==2,"handshake frames");
  google::protobuf::Struct subscribe,authorize;
  check(google::protobuf::util::JsonStringToMessage(frames[0],&subscribe).ok(),"subscribe JSON");
  check(google::protobuf::util::JsonStringToMessage(frames[1],&authorize).ok(),"authorize JSON");
  auto sid=std::to_string(static_cast<unsigned>(subscribe.fields().at("id").number_value()));
  auto aid=std::to_string(static_cast<unsigned>(authorize.fields().at("id").number_value()));
  check(client.process_inbound_line("{\"id\":"+sid+",\"result\":[true,\"KaspaStratum/1.0.0\"]}"),"banner subscription");
  check(client.process_inbound_line("{\"method\":\"set_extranonce\",\"params\":[\"0102\",6]}"),"bare extranonce");
  check(client.process_inbound_line("{\"id\":"+aid+",\"result\":true}"),"authorize");
  check(client.process_inbound_line("{\"method\":\"mining.set_difficulty\",\"params\":[0.005]}"),"float difficulty");
  auto notify=[&](const std::array<std::uint64_t,4>& words) {
   std::string wire="{\"method\":\"mining.notify\",\"params\":[\"synthetic\",[";
   for(unsigned i=0;i<4;++i){if(i)wire+=',';wire+=std::to_string(words[i]);}
   wire+="],1794413398646,150538576,489221514]}";
   client.process_inbound_line(wire);auto job=client.current_job();check(job.has_value(),"compact job missing");
   for(unsigned w=0;w<4;++w)for(unsigned b=0;b<8;++b)
    check(job->pre_pow[w*8+b]==static_cast<std::uint8_t>(words[w]>>(8*b)),"u64 hash word rounded or wrong endian");
   check(job->protocol_version==3 && job->header.daa_score==150538576 && job->header.timestamp==1794413398646 && job->header.bits==489221514 && job->header.pom_tier==0,"worker work fields missing");
   check(job->nonce_fixed==0x0102000000000000ULL && job->nonce_mask==0x0000ffffffffffffULL,"nonce allocation mismatch");
   check(job->difficulty==0.005,"difficulty lexeme changed");return job->generation;
  };
  auto generation=notify({9007199254740993ULL,9223372036854775808ULL,UINT64_MAX,0});
  client.process_inbound_line("{\"method\":\"set_extranonce\",\"params\":[\"0102\",6]}");
  check(client.current_job()->generation==generation,"identical allocation invalidated work");
  client.process_inbound_line("{\"method\":\"mining.set_extranonce\",\"params\":[\"0103\",6]}");
  check(client.current_job()->generation!=generation && client.current_job()->nonce_fixed==0x0103000000000000ULL,"changed allocation not invalidated");
  client.process_inbound_line("{\"method\":\"set_extranonce\",\"params\":[\"0102\",6]}");
  std::mt19937_64 random(42);
  for(unsigned i=0;i<2000;++i)notify({random(),random(),random(),random()});
  for(const auto *bad:{"18446744073709551616","-1","1.5","1e19"}) {
   bool rejected=false;try{client.process_inbound_line(std::string("{\"method\":\"mining.notify\",\"params\":[\"invalid\",[")+bad+",0,0,0],1,150538576,489221514]}");}catch(const std::exception&){rejected=true;}
   check(rejected,"invalid u64 accepted");
  }
  const std::string strings=R"({"text":"18446744073709551615\" still text","fraction":0.005,"exponent":1e19})";
  check(preserve_json_integers(strings)==strings,"strings or float syntax changed");
  for(const auto *invalid:{"[018446744073709551615]","{18446744073709551615:0}","[18446744073709551615 0]"}) {
   google::protobuf::Value parsed;check(!google::protobuf::util::JsonStringToMessage(preserve_json_integers(invalid),&parsed).ok(),"invalid JSON repaired");
  }
  client.process_inbound_line("{\"id\":"+sid+",\"result\":[false,\"KaspaStratum/1.0.0\"]}");
  check(client.telemetry_snapshot().state=="ERROR","false subscription accepted");
  std::cout<<"COMPACT_STRATUM=PASS numeric_u64_words=8004 lossless=true endian=little nonce_alias=true work_fields=true invalid_numbers_rejected=true\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
