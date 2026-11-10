#include "keryx/nexus/pool_telemetry.hpp"
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace keryx::nexus::stratum {
namespace {
std::string quoted(std::string_view s) {
  if (s.size() > 256) throw std::invalid_argument("pool metadata string bound");
  std::string out="\"";
  for(unsigned char c:s) {
    if(c=='"'||c=='\\') { out+='\\';out+=static_cast<char>(c); }
    else if(c<32) {out+="\\u00";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}
    else out+=static_cast<char>(c);
  }
  return out+'"';
}
void validate(const PoolTelemetryOptions &c) {
  if(c.devices.size()>16||c.tier>4||(!c.model_id.empty() &&
      (c.model_id.size()!=64||c.model_id.find_first_not_of("0123456789abcdef")!=std::string::npos)))
    throw std::invalid_argument("pool telemetry identity bounds");
  bool indices[16]{};
  for(const auto &g:c.devices) {
    if(g.index>=16||indices[g.index])throw std::invalid_argument("pool telemetry device mapping");
    indices[g.index]=true;
  }
}
}
std::string pool_hello_json(const PoolTelemetryOptions &c) {
  validate(c);
  std::ostringstream out;
  out<<"{\"v\":1,\"miner\":{\"name\":\"nexus-keryx-miner\",\"version\":\""<<nexus_pool_version
     <<"\",\"build\":"<<quoted(c.build)<<",\"features\":[\"pom-v5\"]},\"trace\":"<<(c.trace_ready?1:0)
     <<",\"model\":{\"loaded\":"<<(c.model_id.empty()?"false":"true");
  if(!c.model_id.empty())out<<",\"tier\":"<<c.tier<<",\"model_id\":"<<quoted(c.model_id);
  out<<"}";
  if(!c.devices.empty()) {
    out<<",\"gpus\":[";bool first=true;
    for(const auto &g:c.devices) {
      if(!first)out<<',';first=false;
      out<<"{\"idx\":"<<g.index;
      if(!g.vendor.empty())out<<",\"vendor\":"<<quoted(g.vendor);
      if(!g.model.empty())out<<",\"model\":"<<quoted(g.model);
      out<<'}';
    }
    out<<']';
  }
  out<<'}';
  auto text=out.str();if(text.size()>8192)throw std::invalid_argument("pool hello size bound");return text;
}
std::string pool_metrics_json(const PoolTelemetryOptions &c,std::uint64_t uptime,
    std::optional<std::uint64_t> rate,std::uint64_t acc,std::uint64_t rej,std::uint64_t stale,bool busy) {
  validate(c);
  std::ostringstream out;
  out<<"{\"v\":1,\"uptime_s\":"<<uptime<<",\"trace\":"<<(c.trace_ready?1:0)
     <<",\"local_shares\":{\"acc\":"<<acc<<",\"rej\":"<<rej<<",\"stale\":"<<stale<<'}';
  if(rate)out<<",\"hashrate_total\":"<<*rate<<",\"rate_basis\":\"completed_pom_nonce_walks_per_second\"";
  out<<",\"busy\":"<<(busy?"true":"false")<<",\"models\":[";
  if(c.trace_ready&&!c.model_id.empty())out<<quoted(c.model_id);
  out<<"]}";return out.str();
}
} // namespace keryx::nexus::stratum
