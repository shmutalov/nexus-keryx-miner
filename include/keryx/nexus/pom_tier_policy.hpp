#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace keryx::nexus {
// Only verified local model/tree/runtime bindings belong in this catalog.
struct QualifiedPomModel {
  std::string id;
  unsigned tier{};
  bool downloaded{}, qualified{};
  std::uint64_t first_daa{}, last_daa{UINT64_MAX};
  bool ready(std::uint64_t daa) const { return downloaded && qualified && first_daa<=daa && daa<=last_daa; }
};
class PomTierPolicy {
public:
  struct Assignment { unsigned effective_tier{}; std::string model; };
  explicit PomTierPolicy(std::vector<QualifiedPomModel> catalog):catalog_(std::move(catalog)) {
    std::set<std::string> ids;
    for(const auto &m:catalog_)
      if(m.tier>4 || m.id.size()!=64 || m.first_daa>m.last_daa || !ids.insert(m.id).second)
        throw std::invalid_argument("invalid qualified PoM catalog");
  }
  void bind(std::string device, std::string_view model, std::uint64_t daa) {
    const auto it=std::find_if(catalog_.begin(),catalog_.end(),[&](const auto &m){return m.id==model&&m.ready(daa);});
    if(it==catalog_.end() || banned_[device].contains(it->id))throw std::invalid_argument("unqualified PoM model assignment");
    assignments_[std::move(device)]={it->tier,it->id};
  }
  std::optional<Assignment> assignment(std::string_view device) const {
    const auto it=assignments_.find(std::string(device));return it==assignments_.end()?std::nullopt:std::optional(it->second);
  }
  std::optional<Assignment> after_oom(std::string_view device, std::uint64_t daa) {
    auto it=assignments_.find(std::string(device));if(it==assignments_.end())return {};
    const auto failed=it->second;banned_[it->first].insert(failed.model);
    const QualifiedPomModel *pick=nullptr;
    for(const auto &m:catalog_)
      if(m.ready(daa)&&m.tier<failed.effective_tier&&!banned_[it->first].contains(m.id)&&(!pick||m.tier>pick->tier))pick=&m;
    if(!pick){assignments_.erase(it);return {};}
    it->second={pick->tier,pick->id};return it->second;
  }
  void advance_era(std::uint64_t daa) {
    for(auto it=assignments_.begin();it!=assignments_.end();) {
      const QualifiedPomModel *pick=nullptr;
      for(const auto &m:catalog_)
        if(m.tier==it->second.effective_tier&&m.ready(daa)&&!banned_[it->first].contains(m.id)&&(!pick||m.first_daa>pick->first_daa))pick=&m;
      if(!pick)it=assignments_.erase(it);
      else{it->second.model=pick->id;++it;}
    }
  }
  std::set<std::string> served_models() const {
    std::set<std::string> out;for(const auto &[device,a]:assignments_)out.insert(a.model);return out;
  }
  bool model_in_use(std::string_view id) const { return served_models().contains(std::string(id)); }
private:
  std::vector<QualifiedPomModel> catalog_;
  std::map<std::string,Assignment> assignments_;
  std::map<std::string,std::set<std::string>> banned_;
};
}
