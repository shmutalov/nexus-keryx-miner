#pragma once
#include <atomic>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace keryx::nexus {
// Orthogonal mining/service states. This controller cannot attest canonical duties.
struct PomPauseCancelled {};

struct ExitBarrierResult {
  bool safe{false};
  std::string reason;
};

class PomH15Lifecycle {
public:
 enum class Pom { Running, Pausing, Paused };
 enum class Service { Active, Draining, RecoveryRequired };
 using ExitBarrier = std::function<ExitBarrierResult()>;

 explicit PomH15Lifecycle(std::filesystem::path path):path_(std::move(path)){}
 bool cancel_pom() { poll();return paused_.load(std::memory_order_acquire); }
 void drain(){std::lock_guard l(mutex_);drain_locked(false);}
 void pause_for_h15_recovery(){std::lock_guard l(mutex_);recovery_paused_=true;drain_locked(true);}
 void clear_h15_recovery(){
   std::lock_guard l(mutex_);
   if(!recovery_paused_)return;
   recovery_paused_=false;
   if(error_.empty()&&(sequence_==0||last_.ends_with(" RESUME"))){
     paused_.store(false,std::memory_order_release);
     pom_.store(Pom::Running);
     service_.store(Service::Active);
   }
 }
 std::uint64_t epoch() const{return epoch_.load(std::memory_order_acquire);}
 bool cancel_pom(std::uint64_t epoch){return cancel_pom()||epoch!=this->epoch();}
 bool paused() const {return paused_.load(std::memory_order_acquire);}
 template<class Release> bool settle_pause(Release release){
   const auto before=epoch();
   if(!paused())return false;
   if(pom_.load()==Pom::Paused)return true;
   // Never hold the control mutex while waiting for GPU ownership: GPU work
   // may itself query cancellation. Recheck the epoch after the drain.
   if(!release())return false;
   std::lock_guard l(mutex_);
   if(paused()&&before==epoch())pom_.store(Pom::Paused);
   return paused()&&pom_.load()==Pom::Paused;
 }
 template<class Poll,class Requests,class Advance,class Submit> void service_tick(Poll poll,Requests requests,Advance advance,Submit submit){
   // Unconditional, including PAUSED, DRAINING, RECOVERY_REQUIRED and no VRAM.
   poll();requests();advance();submit();
 }
 void set_exit_barrier(ExitBarrier barrier) {
   std::lock_guard l(mutex_);
   exit_barrier_ = std::move(barrier);
 }
 bool safe_to_exit() const {
   std::string reason;
   return safe_to_exit(reason);
 }
 bool safe_to_exit(std::string &reason) const {
   if (!paused_.load(std::memory_order_acquire)) {
     reason = "POM_NOT_PAUSED";
     return false;
   }
   if (pom_.load(std::memory_order_acquire) != Pom::Paused) {
     reason = "POM_DRAINING_NOT_PAUSED";
     return false;
   }
   if (service_.load(std::memory_order_acquire) == Service::RecoveryRequired) {
     reason = "H15_RECOVERY_REQUIRED";
     return false;
   }
   std::lock_guard l(mutex_);
   if (!error_.empty()) {
     reason = "LIFECYCLE_ERROR: " + error_;
     return false;
   }
   if (!exit_barrier_) {
     reason = "BARRIER_UNAVAILABLE";
     return false;
   }
   const auto result = exit_barrier_();
   if (!result.safe) {
     reason = result.reason.empty() ? "BARRIER_REJECTED" : result.reason;
     return false;
   }
   reason = "VERIFIED_ALL_OBLIGATIONS_CLEARED";
   return true;
 }
 std::string exit_block_reason() const {
   std::string reason;
   safe_to_exit(reason);
   return reason;
 }
 void service_unavailable(){std::lock_guard l(mutex_);invalid("H15_SERVICE_UNAVAILABLE");}
 const char* operational_mode()const{
   if(pom_.load()!=Pom::Paused)return paused()?"POM_TRANSITION":"POM_AND_H15_ACTIVE";
   return service_.load()==Service::RecoveryRequired?"POM_DISABLED_H15_RECOVERY_REQUIRED":"POM_DISABLED_H15_ACTIVE";
 }
 bool pom_phase_finished()const{return pom_.load()==Pom::Paused&&paused();}
 const char* pom_state() const {switch(pom_.load()){case Pom::Running:return "POM_RUNNING";case Pom::Pausing:return "POM_PAUSING";default:return "POM_PAUSED";}}
 const char* service_state() const {switch(service_.load()){case Service::Active:return "H15_ACTIVE";case Service::Draining:return "H15_DRAINING";default:return "H15_RECOVERY_REQUIRED";}}
 std::string error()const {std::lock_guard l(mutex_);return error_;}
private:
 void drain_locked(bool recovery){if(!paused_.load())epoch_.fetch_add(1,std::memory_order_acq_rel);paused_.store(true,std::memory_order_release);if(pom_.load()==Pom::Running)pom_.store(Pom::Pausing);service_.store(recovery?Service::RecoveryRequired:Service::Draining);}
 void invalid(const char*e){error_=e;drain_locked(true);}
 void poll(){
   std::lock_guard l(mutex_);const auto now=std::chrono::steady_clock::now();
   if(now<next_)return;next_=now+std::chrono::milliseconds(20);
   const int fd=::open(path_.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
   if(fd<0){if(errno!=ENOENT||sequence_!=0)invalid("CONTROL_UNAVAILABLE");return;}
   struct Close{int fd;~Close(){::close(fd);}}close{fd};struct stat a{},b{};
   if(::fstat(fd,&a)||!S_ISREG(a.st_mode)||a.st_uid!=::getuid()||(a.st_mode&0077)||a.st_size<=0||a.st_size>64){invalid("CONTROL_UNSAFE");return;}
   char buf[65];const auto n=::read(fd,buf,sizeof(buf));
   if(n<=0||n>64||::fstat(fd,&b)||a.st_size!=n||a.st_size!=b.st_size||a.st_mtim.tv_sec!=b.st_mtim.tv_sec||a.st_mtim.tv_nsec!=b.st_mtim.tv_nsec){invalid("CONTROL_CHANGED_OR_TRUNCATED");return;}
   std::string value(buf,n);if(!value.empty()&&value.back()=='\n')value.pop_back();
   const auto space=value.find(' ');std::uint64_t seq=0;
   if(space==std::string::npos){invalid("CONTROL_ENCODING");return;}
   const auto result=std::from_chars(value.data(),value.data()+space,seq);
   const auto command=value.substr(space+1);
   if(result.ec!=std::errc{}||result.ptr!=value.data()+space||seq==0||(command!="PAUSE"&&command!="DRAIN"&&command!="RESUME"&&command!="EXIT"&&command!="H15_ONLY")){invalid("CONTROL_ENCODING");return;}
   if(seq<sequence_||(seq==sequence_&&value!=last_)){invalid("CONTROL_REPLAY_OR_CONFLICT");return;}
   if(seq==sequence_)return;
   sequence_=seq;last_=value;error_.clear();
   if(command=="RESUME"){paused_.store(false,std::memory_order_release);pom_.store(Pom::Running);service_.store(Service::Active);}
   else if(command=="EXIT"){
     if(paused_.load(std::memory_order_relaxed)&&pom_.load(std::memory_order_relaxed)==Pom::Paused&&exit_barrier_){
       const auto barrier_res=exit_barrier_();
       if(barrier_res.safe){
         service_.store(Service::Draining);
       } else {
         invalid(("EXIT_BARRIER_DENIED: " + (barrier_res.reason.empty()?"UNPROVEN":barrier_res.reason)).c_str());
       }
     } else {
       invalid("EXIT_CANONICAL_AND_RECOVERY_BARRIER_UNAVAILABLE");
     }
   }
   else {drain_locked(false);if(command=="PAUSE")service_.store(Service::Active);}
 }
 std::filesystem::path path_;mutable std::mutex mutex_;
 std::chrono::steady_clock::time_point next_{};
 std::atomic_uint64_t epoch_{0};
 std::atomic_bool paused_{false};std::atomic<Pom> pom_{Pom::Running};
 std::atomic<Service> service_{Service::Active};std::uint64_t sequence_=0;
 bool recovery_paused_{false};
 std::string last_,error_;
 ExitBarrier exit_barrier_;
};
}
