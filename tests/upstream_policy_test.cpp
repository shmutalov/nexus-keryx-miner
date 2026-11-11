#include "keryx/nexus/ai_sibling_schedule.hpp"
#include "keryx/nexus/pom_tier_policy.hpp"
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus;
void check(bool b,const char *m){if(!b)throw std::runtime_error(m);}
std::string id(char c){return std::string(64,c);}
int main(){try{
 using ai::sibling_wait_ms;
 check(sibling_wait_ms(0,10000,1,1000000,0)==0,"first sibling waited");
 check(sibling_wait_ms(1,100,1,1000000,25'000'000'000ULL)==0,"generation-sized wait not elapsed");
 check(sibling_wait_ms(1,10000,1,1000000,25'000'000'000ULL)>0,"prompt processing omitted");
 check(sibling_wait_ms(1,10000,1000,2000,0)==0,"near deadline delayed");
 check(sibling_wait_ms(1,UINT64_MAX,UINT64_MAX-1,UINT64_MAX,0)==0,"urgency overflowed");
 check(sibling_wait_ms(2,100,1,0,0)==0,"unknown deadline delayed");
 PomTierPolicy p({{id('a'),3,true,true,0,19},{id('b'),3,true,true,20},
                  {id('c'),1,true,true,0,19},{id('d'),1,true,true,20},
                  {id('e'),0,true,true},{id('f'),2,false,true},
                  {id('1'),2,true,false}});
 p.bind("gpu-A",id('a'),10);p.bind("gpu-B",id('a'),10);
 auto a=p.after_oom("gpu-A",10);check(a&&a->model==id('c'),"download/qualification filter or largest-smaller choice");
 check(p.model_in_use(id('a')),"shared model released while another device uses it");
 check(p.served_models()==std::set<std::string>{id('a'),id('c')},"served set not actual assignments");
 a=p.after_oom("gpu-A",10);check(a&&a->model==id('e'),"downloaded but unserved smaller model ignored");
 p.advance_era(20);check(p.assignment("gpu-A")->effective_tier==0,"OOM downgrade reverted on era change");
 check(p.assignment("gpu-B")->model==id('b'),"era-correct model missing");
 check(!p.after_oom("gpu-A",20)&&!p.assignment("gpu-A"),"exhausted device retained");
 check(p.served_models()==std::set<std::string>{id('b')},"failed device advertised");
 bool denied=false;try{p.bind("gpu-A",id('e'),20);}catch(const std::invalid_argument&){denied=true;}
 check(denied,"OOM model immediately rebound");
 std::cout<<"UPSTREAM_POLICY=PASS prompt_wait=true deadline_override=true smaller_downloaded=true qualified_only=true sticky_tier=true served_union=true shared_lifetime=true no_smaller_withdraw=true\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
