#include "keryx/nexus/stratum_v4_client.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus;
// Synthetic RFC1918 test octets; never installation addresses.
std::string ip(std::array<unsigned,4> octets) {
  return std::to_string(octets[0])+"."+std::to_string(octets[1])+"."+std::to_string(octets[2])+"."+std::to_string(octets[3]);
}
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class MissingBlock final : public protowire::RPC::Service {
  grpc::Status MessageStream(grpc::ServerContext*, grpc::ServerReaderWriter<protowire::KaspadResponse, protowire::KaspadRequest>* stream) override {
    protowire::KaspadRequest request;
    while (stream->Read(&request)) {
      protowire::KaspadResponse response; response.set_id(request.id());
      response.mutable_getblockresponse()->mutable_error()->set_message("block not found");
      if (!stream->Write(response)) break;
    }
    return grpc::Status::OK;
  }
};
int main(int argc, char** argv) {
  try {
    const std::string mode=argc>1?argv[1]:"all";
    if(mode=="tls_probe") {
      require(argc==4,"tls_probe needs URL and expected result");
      stratum::StratumClientConfig config; config.pool_url=argv[2]; config.worker="synthetic";
      if (const char *ca = std::getenv("NEXUS_POOL_CA_FILE"); ca && *ca) config.tls_ca_file = ca;
      config.connect_timeout_ms=1000; config.io_timeout_ms=1000;
      stratum::StratumV4Client client(config);
      require(client.connect_and_handshake_once()==(std::string(argv[3])=="accept"),"TLS authentication result mismatch");
    }
    if(mode=="tls_stop") {
      require(argc==3 || argc==4,"tls_stop needs URL and optional iteration count");
      const auto iterations=argc==4?std::stoul(argv[3]):100;
      require(iterations>0 && iterations<=10000,"invalid TLS stress iterations");
      for(unsigned i=0;i<iterations;++i) {
        stratum::StratumClientConfig config; config.pool_url=argv[2]; config.worker="synthetic";
      if (const char *ca = std::getenv("NEXUS_POOL_CA_FILE"); ca && *ca) config.tls_ca_file = ca;
        config.connect_timeout_ms=500; config.io_timeout_ms=500;
        stratum::StratumV4Client client(config); client.start();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!client.telemetry_snapshot().connected && std::chrono::steady_clock::now()<deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(client.telemetry_snapshot().connected,"TLS stop fixture never connected");
        std::thread other_stop([&]{client.stop();});
        client.stop();
        other_stop.join();
      }
      std::cout<<"TLS_STOP_CHECKS="<<iterations<<"\n";
    }
    if(mode=="all" || mode=="parser") {
      stratum::StratumClientConfig config; config.pool_url="stratum+tcp://127.0.0.1:1"; config.worker="synthetic";
      stratum::StratumV4Client client(config); client.enable_loopback_test_mode(true);
      unsigned checks=0;
      for(const auto* id : {"0", "1", "9007199254740991", "9007199254740993", "18446744073709551615", "\"18446744073709551615\""}) {
        client.process_inbound_line(std::string("{\"method\":\"ping\",\"id\":")+id+"}");
        require(client.take_outbound_wire_log().size()==1,"valid integer rejected"); ++checks;
      }
      for(const auto* id : {"1.5","-1","18446744073709551616","1e19", "\"-1\"","\"+1\"","\" 1\"","\"1 \"","\"18446744073709551616\"","\"\"","\"1.0\""}) {
        client.process_inbound_line(std::string("{\"method\":\"ping\",\"id\":")+id+"}");
        require(client.take_outbound_wire_log().empty(),"invalid or inexact integer accepted"); ++checks;
      }
      std::uint64_t sample=0x123456789abcdef0ULL;
      for(unsigned i=0;i<1000;++i) {
        sample^=sample<<13;sample^=sample>>7;sample^=sample<<17;
        client.process_inbound_line("{\"method\":\"ping\",\"id\":\""+std::to_string(sample)+"\"}");
        const auto reply=client.take_outbound_wire_log();
        require(reply.size()==1 && reply.front().find(std::to_string(sample))!=std::string::npos,"decimal u64 round trip lost precision");
      }
      std::cout<<"PARSER_CHECKS="<<checks<<" U64_ROUNDTRIPS=1000\n";
    }
    if(mode=="all" || mode=="unknown") {
      MissingBlock service; grpc::ServerBuilder builder; int port=0;
      builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port); builder.RegisterService(&service);
      auto server=builder.BuildAndStart(); require(bool(server),"mock server failed");
      network::SubmitBlockClient client("grpc://127.0.0.1:"+std::to_string(port));
      for(auto state:{network::SubmissionState::UnknownOutcome,network::SubmissionState::UnknownAfterTimeout,network::SubmissionState::RpcAcknowledged}) {
        network::SubmitBlockResult candidate; candidate.state=state; candidate.block_hash=std::string(64,'a'); candidate.transitions.push_back(state);
        const auto result=client.observe_chain(candidate,std::chrono::seconds(1),std::chrono::milliseconds(10),6);
        require(result.state==state,"absent chain evidence fabricated an RPC ACK");
        require(result.transitions==candidate.transitions,"absent chain evidence changed history");
      }
      network::SubmitBlockResult unknown; unknown.state=network::SubmissionState::UnknownOutcome;
      unknown.block_hash=std::string(64,'b');
      std::atomic_bool cancelled=false;
      std::jthread cancellation([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));cancelled=true;});
      const auto started=std::chrono::steady_clock::now();
      const auto stopped=client.observe_chain(unknown,std::chrono::seconds(30),std::chrono::seconds(5),6,
          [&]{return cancelled.load();});
      require(stopped.state==unknown.state,"cancellation fabricated a transport result");
      require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(500),"observer sleep ignored cancellation");
      server->Shutdown(); std::cout<<"UNKNOWN_CHECKS=3 CANCELLATION_CHECKS=1\n";
    }
    if(mode=="all" || mode=="upload") {
      stratum::StratumClientConfig config; config.pool_url="stratum+tcp://127.0.0.1:1"; config.worker="synthetic";
      stratum::StratumV4Client client(config);
      for(const auto& address:std::vector<std::string>{"0.0.0.0",ip({10,1,2,3}),"100.64.0.1","127.0.0.1","169.254.169.254","172.16.0.1","172.31.255.255",ip({192,168,0,1}),"198.18.0.1","192.0.2.1","224.0.0.1","255.255.255.255","::","::1","::ffff:127.0.0.1","fe80::1","fd00::1","2001:db8::1","2002:7f00:1::1"})
        require(!stratum::is_public_trace_ip(address),"nonpublic trace IP admitted");
      for(const auto* address:{"8.8.8.8","1.1.1.1","2606:4700:4700::1111"})
        require(stratum::is_public_trace_ip(address),"public trace IP rejected");
      const std::string id(64,'a'); const std::array<std::uint8_t,4> blob{1,2,3,4};
      for(const auto& url:std::vector<std::string>{"http://127.0.0.1:1", "https://127.0.0.1:1", "https://[::1]:1", "https://"+ip({10,0,0,1})+":1", "https://169.254.169.254:1", "https://user:pass@127.0.0.1:1"}) {
        client.register_trace_upload_url(id,url);
        require(!client.upload_trace_record(id,blob),"unsafe trace upload accepted");
      }
      std::cout<<"UPLOAD_NEGATIVE_CHECKS=6 IP_POLICY_CHECKS=22\n";
    }
    if(mode=="all" || mode=="tls_default") require(stratum::StratumClientConfig{}.tls_verify_peer,"TLS verification disabled by default");
    std::cout<<"NETWORK_SECURITY=PASS mode="<<mode<<"\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
