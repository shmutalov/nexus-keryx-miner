//! Additive diagnostics. Canonical signed states remain in schema4 audit files.
//! This ledger never authorizes effects, seed changes, expiry or chain acceptance.
use super::*;
use serde::{Serialize,Deserialize};
#[derive(Default,Serialize,Deserialize)]
#[serde(default)]
struct Ledger {
    rpc: BTreeMap<String,(u8,u64,u64)>, // outcome ACK=0 REJECT=1 UNKNOWN=2, wall, latency
    seen: BTreeMap<String,BTreeMap<String,u64>>,
    timings: BTreeMap<String,Vec<u64>>,
}
impl Service {
    fn read_health(&self)->Result<Ledger,String>{
        let path=self.dir.join("health.json");
        if !path.exists(){return Ok(Ledger::default());}
        let meta=fs::symlink_metadata(&path).map_err(|e|e.to_string())?;
        if !meta.is_file() || meta.len()>16<<20{return Err("health ledger bounds".into());}
        let b=fs::read(path).map_err(|e|e.to_string())?;
        if b.len()<32 || blake3::hash(&b[32..]).as_bytes()!=&b[..32]{return Err("health checksum; original retained".into());}
        serde_json::from_slice(&b[32..]).map_err(|_|"health ledger encoding".into())
    }
    fn write_health(&self,h:&Ledger)->Result<(),String>{
        let b=serde_json::to_vec(h).map_err(|_|"health encoding")?;
        if b.len()>16<<20{return Err("health ledger bound; retained".into());}
        let mut file=tempfile::NamedTempFile::new_in(&self.dir).map_err(|e|e.to_string())?;
        file.write_all(blake3::hash(&b).as_bytes()).and_then(|_|file.write_all(&b)).and_then(|_|file.as_file().sync_all()).map_err(|e|e.to_string())?;
        file.persist(self.dir.join("health.json")).map_err(|e|e.to_string())?;
        fs::File::open(&self.dir).and_then(|f|f.sync_all()).map_err(|e|e.to_string())
    }
    pub fn rpc_observed(&self,id:Hash,outcome:u8,latency:u64)->Result<(),String>{
        if outcome>2{return Err("RPC outcome bounds".into());}
        // Outcomes can only reference immutable canonical bytes already persisted.
        let mut tracked=false;
        for entry in fs::read_dir(&self.dir).map_err(|e|e.to_string())? {
            let p=entry.map_err(|e|e.to_string())?.path();
            if p.extension().is_some_and(|x|x=="audit") {
                let s=read_state(&p)?;
                tracked |= s.response==id || s.pieces.iter().any(|x|x.id==id);
            }
        }
        if !tracked{return Err("untracked RPC outcome".into());}
        let mut h=self.read_health()?;h.rpc.insert(hex::encode(id),(outcome,wall_ms(),latency));
        if latency>0{let values=h.timings.entry("rpc".into()).or_default();values.push(latency);if values.len()>512{values.remove(0);}}
        self.write_health(&h)
    }
    pub fn observe_timing(&self,phase:&str,ms:u64)->Result<(),String>{
        if !["replay","signing"].contains(&phase){return Err("timing phase bounds".into());}
        let mut h=self.read_health()?;let values=h.timings.entry(phase.to_string()).or_default();values.push(ms);if values.len()>512{values.remove(0);}self.write_health(&h)
    }
    pub fn health(&self,now:u64)->Result<Vec<u8>,String>{
        let mut ledger=self.read_health()?;let previous=serde_json::to_vec(&ledger).unwrap();
        let ms=wall_ms();let mut rows=Vec::new();let mut samples:BTreeMap<String,Vec<u64>>=BTreeMap::new();
        for entry in fs::read_dir(&self.dir).map_err(|e|e.to_string())? {
            let path=entry.map_err(|e|e.to_string())?.path();
            if !path.extension().is_some_and(|x|x=="audit"){continue;}
            let stored=read_state(&path)?;
            let s=self.pending.get(&stored.response).map_or(&stored,|p|&p.state);
            let id=hex::encode(s.response);let mut stages=vec!["RESPONSE_PREPARED"];
            if s.accepted.is_some() || ledger.rpc.get(&id).is_some_and(|x|x.0==0){stages.push("RESPONSE_SUBMITTED");}
            if ledger.rpc.get(&id).is_some_and(|x|x.0==0){stages.push("RPC_ACKNOWLEDGED");}
            if self.restore.is_empty() && s.accepted.is_some(){stages.push("CHAIN_ACCEPTED");}
            if self.restore.is_empty() && s.seed.is_some(){stages.push("TRACE_SEEDED");stages.push("OPENING_REQUIRED");}
            if !s.pieces.is_empty(){stages.push("OPENING_BUILT");}
            if s.pieces.iter().any(|x|x.accepted.is_some() || ledger.rpc.get(&hex::encode(x.id)).is_some_and(|r|r.0==0)){stages.push("OPENING_SUBMITTED");}
            if !s.pieces.is_empty() && s.pieces.iter().all(|x|ledger.rpc.get(&hex::encode(x.id)).is_some_and(|r|r.0==0)){stages.push("OPENING_RPC_ACKNOWLEDGED");}
            let accepted=self.restore.is_empty() && !s.pieces.is_empty()&&s.pieces.iter().all(|x|x.accepted.is_some());
            if accepted{stages.push("OPENING_CHAIN_ACCEPTED");}
            let complete=self.restore.is_empty() && s.accepted.is_some()&&!unfinished(&s.pieces)&&self.node_awaits.get(&s.response)!=Some(&true);
            if complete{stages.push("AUDIT_COMPLETE");}
            let deadline=s.policy.node.as_ref().map(|x|x.deadline).or(s.policy.deadline).or_else(||s.pieces.iter().filter(|x|x.accepted.is_none()).map(|x|x.deadline).min());
            let margin=deadline.map(|d|d as i128-now as i128);
            let unknown=s.pieces.iter().any(|x|x.attempts>0&&x.accepted.is_none()&&ledger.rpc.get(&hex::encode(x.id)).is_none_or(|r|r.0==2));
            let status=if complete{"OPENING_ACCEPTED"}else if s.retired&&!s.policy.unaudited || deadline.is_some_and(|d|now>d){"OPENING_EXPIRED"}else if !self.restore.is_empty()||unknown||s.failures>0{"OPENING_RECOVERING"}else if margin.is_some_and(|m|m<=1500){"OPENING_AT_RISK"}else{"OPENING_PENDING"};
            let times=ledger.seen.entry(id.clone()).or_default();
            for stage in &stages{times.entry(stage.to_string()).or_insert(ms);}
            let mut latencies=BTreeMap::new();
            for (a,b,name) in [("RESPONSE_PREPARED","CHAIN_ACCEPTED","response_to_chain_observed"),("OPENING_REQUIRED","OPENING_BUILT","replay_observed"),("OPENING_SUBMITTED","OPENING_CHAIN_ACCEPTED","opening_to_chain_observed")]{
                if let (Some(a),Some(b))=(times.get(a),times.get(b)){let duration=b.saturating_sub(*a);latencies.insert(name,duration);samples.entry(name.to_string()).or_default().push(duration);}
            }
            rows.push(serde_json::json!({"obligation":&id[..14],"chain_coverage_complete":self.restore.is_empty(),"response_submit_intent":s.response_attempts>0,"opening_submit_intent":s.pieces.iter().any(|x|x.attempts>0),"status":status,"stage":stages.last(),"observed_stages":times,"deadline_daa":deadline,"margin_daa":margin.map(|m|m.to_string()),"deadline_authoritative":s.policy.node.is_some(),"response_rpc":ledger.rpc.get(&id).map(|r|["ACK","REJECT","UNKNOWN"][r.0 as usize]).unwrap_or(if s.response_attempts>0{"UNKNOWN"}else{"NOT_SENT"}),"phase_latency_ms":latencies,"retired_record_retained":s.retired}));
        }
        for (name,values) in &ledger.timings{samples.entry(name.clone()).or_default().extend(values);}
        let mut percentiles=BTreeMap::new();
        for (name,mut values) in samples{values.sort_unstable();let n=values.len();let q=|p:usize|values[((n*p).div_ceil(100)).saturating_sub(1).min(n-1)];percentiles.insert(name,serde_json::json!({"samples":n,"p50":q(50),"p95":q(95),"p99":q(99)}));}
        if previous!=serde_json::to_vec(&ledger).unwrap(){self.write_health(&ledger)?;}
        serde_json::to_vec(&serde_json::json!({"event":"H15_OPENING_HEALTH","daa":now,"rows":rows,"latency_observed_ms":percentiles,"measurements":"local observation times; missing historical stages are not reconstructed"})).map_err(|_|"health output".into())
    }
}
