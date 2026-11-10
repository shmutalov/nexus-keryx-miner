#include "keryx/nexus/ai_response_service.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::ai;
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
const std::string req(64,'a'),escrow(64,'b'),identity(64,'c');
class Node final:public protowire::RPC::Service {
public:
 std::atomic_int mode=0;
 grpc::Status MessageStream(grpc::ServerContext*,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>* stream)override{
  protowire::KaspadRequest q;
  while(stream->Read(&q)){
   protowire::KaspadResponse r;r.set_id(q.id());
   if(q.has_getawaitedopeningsrequest()){
    check(q.getawaitedopeningsrequest().escrowpubkey()==escrow,"wrong responder query");
    check(q.getawaitedopeningsrequest().requesthashes_size()==1&&q.getawaitedopeningsrequest().requesthashes(0)==req,"wrong request query");
    auto *p=r.mutable_getawaitedopeningsresponse();p->set_virtualdaascore(10000);p->set_requestschecked(mode!=4);
    if(mode==0||mode==7){auto *v=p->add_awaitedrequests();v->set_requesthash(req);v->set_windowenddaa(mode==7?0:11000);}
    if(mode==1)p->add_closedrequests(req);
    if(mode==5)p->add_closedrequests(std::string(64,'d'));
   }else if(q.has_getservicestrikesrequest()){
    auto *p=r.mutable_getservicestrikesresponse();p->set_virtualdaascore(10010);
    if(mode==3||mode==6||mode==8){auto *v=p->add_pendingburns();v->set_requesthash(req);v->set_miner(mode==6?escrow:identity);v->set_missdaascore(mode==8?20000:9990);}
   }else return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"unexpected RPC");
   if(!stream->Write(r))break;
  }
  return grpc::Status::OK;
 }
};
int main(){try{
 Node node;grpc::ServerBuilder b;int port=0;b.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);b.RegisterService(&node);auto server=b.BuildAndStart();check(bool(server),"server start");
 AiResponseTransport transport("grpc://127.0.0.1:"+std::to_string(port));
 using S=AiRequestAdmission::State;
 for(int mode=0;mode<=8;++mode){node.mode=mode;const auto status=transport.request_admission(req,escrow,identity);
  const auto expected=(mode==0||mode==7)?S::Awaiting:(mode==1||mode==3)?S::Closed:(mode==4||mode==5)?S::Unavailable:S::NotYetKnown;
  check(status.state==expected,"unsafe request admission classification");
  if(mode==0)check(status.deadline_daa==11000,"authoritative deadline lost");
  if(mode==7)check(status.deadline_daa==0,"unarmed duty given an invented deadline");
 }
 check(transport.request_admission("invalid",escrow,identity).state==S::Unavailable,"invalid request admitted");
 server->Shutdown();std::cout<<"AI_REQUEST_ADMISSION=PASS cases=10 checked_response=true absent_is_not_closed=true identity_bound_miss=true\n";
}catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}}
