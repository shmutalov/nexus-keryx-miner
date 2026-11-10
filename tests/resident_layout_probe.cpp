#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/consensus/resident_route.hpp"
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/pom_vram_admission.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
// Manual GPU diagnostic only: invoke under the exclusive test device lease.
// Loads and hashes the official model, never submits a job or calls a node.
int main(int argc,char **argv) {
  try {
    if(argc!=3 && argc!=4 && argc!=7)throw std::invalid_argument("usage: resident-layout-probe RUNTIME MODEL [--search [TREE META ORACLE]]");
    keryx::nexus::opoi::StockLlamaRuntime runtime(argv[1],argv[2]);
    runtime.use_task_context();
    const auto tensors=runtime.resident_tensors();
    std::vector<std::uint64_t> bases,prefix{0};
    std::uint64_t host_bytes=0;
    for(const auto &t:tensors) {
      if(!t.device)host_bytes+=t.bytes;
      bases.push_back(t.address);prefix.push_back(prefix.back()+t.bytes/32);
    }
    const auto layout=keryx::nexus::consensus::inspect_canonical_layout(bases,prefix);
    std::cout<<"RESIDENT_RUNTIME_LAYOUT abi="<<runtime.abi_version()<<" tensors="<<tensors.size()
             <<" canonical_pointer_contiguity="<<(layout.contiguous?"true":"false")
             <<" first_mismatch="<<layout.first_mismatch<<" host_tensor_bytes="<<host_bytes
             <<" actual_search=NOT_RUN inference=NOT_RUN submission=DISABLED\n";
    if(argc>=4) {
      if(std::string_view(argv[3])!="--search") throw std::invalid_argument("unknown probe option");
      using namespace keryx::nexus;
      using namespace keryx::nexus::consensus;
      model::ReadOnlyGguf gguf(argv[2]);
      CudaResidentSearch search(primary_gpu_uuid(),gguf,tensors,runtime.canonical_weights());
      const auto anchor=official_tier_anchor(pom_h16_activation_daa,0);
      constexpr Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
          0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
      if(gguf.chunk_count()!=anchor.chunks)throw std::runtime_error("official model chunk mismatch");
      const auto pad=official_v5_pad(0,model_id,anchor.root,pom_h16_activation_daa);
      if(!search.prepare_v5(pad))throw std::runtime_error("offline pad admission failed");
      Hash pph{};pph.fill(42);Hash target{};
      const auto result=search.run(pph,1700000123,pom_h16_activation_daa,target,2000,4608);
      std::cout<<"OFFLINE_ROUTE_PROBE completed="<<result.completed<<" kernel_ms="<<result.kernel_ms
               <<" candidates="<<bool(result.winner)<<" benchmark=NOT_RUN submission=DISABLED\n";
      if(argc==7) {
        gguf.verify_sidecar(argv[4],argv[5],model_id,anchor.root,anchor.chunks);
        model::SparseMerkleIndex merkle(gguf,argv[4],false);
        constexpr std::uint64_t timestamp=1700000123,nonce=2000,daa=pom_h16_activation_daa;
        const auto seed=block_seed(pph,timestamp,nonce,daa);
        auto read=[&](std::uint64_t tile){std::array<std::uint8_t,tile_bytes> bytes{};gguf.read_tile_into(tile,bytes);return bytes;};
        const auto cpu=walk_v5_from_model(seed,gguf.chunk_count()/32,pad,read);
        const auto diagnostic=search.diagnose_v5(pph,timestamp,daa,nonce);
        if(cpu.states!=diagnostic.states || cpu.offsets!=diagnostic.offsets)
          throw std::runtime_error("real model full state/offset parity failed");
        const auto final=final_state(cpu);const auto pow=pom_pow_value_h3(final,pph);
        if(search.run(pph,timestamp,daa,pow,nonce,1).winner!=nonce)
          throw std::runtime_error("selected route rejected exact CPU output");
        auto below=pow;for(auto &v:below){if(v-- !=0)break;}
        if(search.run(pph,timestamp,daa,below,nonce,1).winner)
          throw std::runtime_error("selected route accepted below CPU output");
        const auto proof=prove_v5_from_walk_parallel(cpu,0,gguf.chunk_count()/32,read,
            [&](std::uint64_t tile){return merkle.tile_path(tile);},4);
        const auto wire=borsh_v5_container(proof,final,pow);
        const ProofArtifactContext context{seed,gguf.chunk_count(),timestamp,nonce,daa,1,final,0,
            anchor.root,pow,pph,{},pow};
        const auto artifacts=encode_proof_artifacts(context,cpu,wire.size());
        RustProofVerifier verifier(argv[6]);
        const auto receipt=verifier.verify(artifacts.context_prefix,artifacts.trace,wire);
        std::cout<<"REAL_MODEL_H16_PARITY=PASS states=257 offsets=256 selected_route_output_checks=2 rust=PASS"
                 <<" rust_ms="<<receipt.cpu_ms<<" inference=NOT_RUN submission=DISABLED\n";
      }

    }
    return 0;
  }catch(const keryx::nexus::PomVramAdmissionDenied &e){
    std::cerr<<e.what()<<" required_pad="<<e.budget.pad_bytes<<" required_metadata="<<e.budget.metadata_bytes
             <<" free="<<e.budget.free_bytes<<" reserve="<<e.budget.reserve_bytes<<"\n";return 2;
  }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
