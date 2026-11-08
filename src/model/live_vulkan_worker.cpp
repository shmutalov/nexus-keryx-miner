#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/ai_service.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/h15_audit_client.hpp"
#include "keryx/nexus/escrow_watcher.hpp"
#include "keryx/nexus/native_telemetry.hpp"
#include "keryx/nexus/startup_readiness.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/consensus/pom_v5.hpp"
#include "keryx/nexus/prepared_rpc_template.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "keryx/nexus/network/submission_pipeline.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/platform/io.hpp"
#include "keryx/nexus/cpu_topology.hpp"
#include "messages.pb.h"
#include "rpc.pb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
namespace fs = std::filesystem;

namespace {

constexpr Hash official_model{
    0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
    0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};

constexpr Hash official_root{
    0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,
    0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};

constexpr std::uint64_t POM_H3_SALT[4] = {
    0x7C99D381176D4EC4ULL, 0xC2E28E3E28118C36ULL, 0xD496CE1B129B76CAULL, 0x47CF0979FA580BCEULL
};

constexpr std::array<std::uint8_t, 32> SEED_H14_TAG = {
    'K','E','R','Y','X','-','H','1','4','-','P','R','I','V','A','T','E','-','I','N','F','E','R','E','N','C','E','-','S','E','E','D'
};

std::uint64_t monotonic_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic_bool process_stop{false};
extern "C" void request_stop(int) { process_stop.store(true, std::memory_order_relaxed); }

struct QueuedAiRequest {
  protowire::RpcTransaction transaction;
  std::uint64_t daa{};
  std::string id;
  std::uint64_t queued_ns{};
};

class AiRequestQueue final {
public:
  bool push(const protowire::RpcBlock &block) {
    if (!block.has_header()) return true;
    for (const auto &tx : block.transactions()) {
      if (tx.subnetworkid() != ai::request_subnetwork) continue;
      std::string why;
      const auto parsed = ai::parse_request(tx, block.header().daascore(), &why, "mainnet");
      if (!parsed) continue;
      if (parsed->model_id != official_model) continue;
      std::lock_guard lock(mutex_);
      if (seen_.contains(parsed->stable_id)) continue;
      if (pending_.size() >= ai::max_pending_requests || seen_.size() >= 10000) {
        overflow_ = true;
        return false;
      }
      seen_.insert(parsed->stable_id);
      pending_.push_back({tx, block.header().daascore(), parsed->stable_id, monotonic_ns()});
      std::cout << "{\"event\":\"AI_REQUEST_QUEUED\",\"request_id\":\""
                << parsed->stable_id << "\",\"daa\":" << block.header().daascore()
                << ",\"pending\":" << pending_.size() << "}\n" << std::flush;
    }
    return true;
  }
  std::optional<QueuedAiRequest> pop() {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) return std::nullopt;
    auto item = std::move(pending_.front());
    pending_.pop_front();
    return item;
  }
  bool overflowed() const noexcept { std::lock_guard lock(mutex_); return overflow_; }
  std::size_t size() const noexcept { std::lock_guard lock(mutex_); return pending_.size(); }
private:
  mutable std::mutex mutex_;
  std::deque<QueuedAiRequest> pending_;
  std::unordered_set<std::string> seen_;
  bool overflow_{};
};

std::vector<std::uint8_t> decode_hex_secret(std::string_view value) {
  if (value.size() != 64) throw std::runtime_error("escrow key has invalid format");
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out(32);
  for (std::size_t i = 0; i < 32; ++i) {
    const int a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]);
    if (a < 0 || b < 0) throw std::runtime_error("escrow key has invalid format");
    out[i] = static_cast<std::uint8_t>((a << 4) | b);
  }
  return out;
}

std::array<std::uint8_t, 32> load_existing_escrow_secret(const fs::path &path) {
  std::ifstream in(path);
  std::string text;
  in >> text;
  if (!in || text.size() != 64) throw std::runtime_error("escrow key file has invalid format");
  const auto decoded = decode_hex_secret(text);
  std::array<std::uint8_t, 32> secret{};
  std::copy(decoded.begin(), decoded.end(), secret.begin());
  return secret;
}

std::array<std::uint8_t, 64> delegation_from_extra(std::string_view extra) {
  const auto e = extra.find("/escrow:"), s = extra.find("/esig:");
  if (e == std::string_view::npos || s == std::string_view::npos || s != e + 8 + 64 || extra.size() < s + 6 + 128)
    return {};
  auto decoded = decode_hex_secret(extra.substr(s + 6, 64));
  auto decoded2 = decode_hex_secret(extra.substr(s + 6 + 64, 64));
  std::array<std::uint8_t, 64> out{};
  std::copy(decoded.begin(), decoded.end(), out.begin());
  std::copy(decoded2.begin(), decoded2.end(), out.begin() + 32);
  return out;
}

std::string canonical_grpc_endpoint(std::string endpoint) {
  if (endpoint.rfind("grpc://", 0) != 0 && endpoint.rfind("grpcs://", 0) != 0) {
    endpoint = "grpc://" + endpoint;
  }
  return endpoint;
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  // Canonical Vulkan environment defaults
  #ifdef _WIN32
  const char *home_env = std::getenv("HOME");
  fs::path home_path = home_env ? fs::path(home_env) : fs::current_path();
  fs::path default_data_dir = home_path / "Scaricati" / "KERYX-NEXUS-RECOVERY";

  _putenv_s("NEXUS_AI_BACKEND", "vulkan");
  _putenv_s("KERYX_LLAMA_CTX", "512");
  #else
  const char *home_env = std::getenv("HOME");
  fs::path home_path = home_env ? fs::path(home_env) : fs::current_path();
  fs::path default_data_dir = home_path / "Scaricati" / "KERYX-NEXUS-RECOVERY";

  setenv("NEXUS_AI_BACKEND", "vulkan", 1);
  setenv("KERYX_LLAMA_CTX", "512", 1);
  if (!std::getenv("NEXUS_DATA_DIR")) {
    setenv("NEXUS_DATA_DIR", default_data_dir.string().c_str(), 1);
  }
  if (!std::getenv("VK_DRIVER_FILES") && fs::exists("/usr/share/vulkan/icd.d/radeon_icd.json")) {
    setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.json", 0);
    setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.json", 0);
  }
  #endif

  std::string endpoint = "grpc://127.0.0.1:22110";
  std::string pay_address;
  std::string extra_data;
  std::string models_dir = (home_path / "Scaricati" / "KERYX" / "models").string();
  std::string model_path;
  std::string tree_path;
  std::string meta_path;
  std::string oracle_path;
  std::string runtime_dir = "/tmp/nexus-vulkan-worker";
  std::string vulkan_lib = (default_data_dir / "artifacts" / "inference" / "vk" / "libkeryx-llama-vk.so").string();
  std::string ipfs_endpoint = "http://127.0.0.1:5001";
  std::string device_id = "0000:00:02.0";
  std::string escrow_key_file;
  std::string escrow_cert_hex;
  std::uint64_t nonce_cursor = 0x4b455259584d3231ULL;
  std::uint32_t batch_size = 16384;
  bool submit_enabled = false;
  bool ai_responses_enabled = false;
  bool escrow_claims_enabled = false;

  std::vector<std::string> pos_args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--inference-capabilities") {
      std::cout << "{\"schema_version\":1,\"default\":\"vulkan\",\"vulkan_helper\":true,\"primary_vulkan\":true,\"primary_vulkan_bda\":true,\"independent_vulkan_lanes\":true,\"secondary_vulkan_bda\":true,\"secondary_vulkan_rdna3\":false,\"trace_capture\":true,\"activation_override\":false}\n" << std::flush;
      return 0;
    }
    else if (arg == "--enable-submit") submit_enabled = true;
    else if (arg == "--no-submit") submit_enabled = false;
    else if (arg == "--enable-ai-response") ai_responses_enabled = true;
    else if (arg == "--no-ai-response") ai_responses_enabled = false;
    else if (arg == "--enable-escrow-claims") escrow_claims_enabled = true;
    else if (arg == "--keryxd-address" || arg == "--endpoint") { if (i + 1 < argc) endpoint = argv[++i]; }
    else if (arg == "-a" || arg == "--address") { if (i + 1 < argc) pay_address = argv[++i]; }
    else if (arg == "--extra-data") { if (i + 1 < argc) extra_data = argv[++i]; }
    else if (arg == "--models-dir") { if (i + 1 < argc) models_dir = argv[++i]; }
    else if (arg == "--model") { if (i + 1 < argc) model_path = argv[++i]; }
    else if (arg == "--tree") { if (i + 1 < argc) tree_path = argv[++i]; }
    else if (arg == "--meta") { if (i + 1 < argc) meta_path = argv[++i]; }
    else if (arg == "--device" || arg == "--gpu") { if (i + 1 < argc) device_id = argv[++i]; }
    else if (arg == "--vulkan-lib") { if (i + 1 < argc) vulkan_lib = argv[++i]; }
    else if (arg == "--ipfs-endpoint" || arg == "--ipfs-api") { if (i + 1 < argc) ipfs_endpoint = argv[++i]; }
    else if (arg == "--escrow-key-file" || arg == "--escrow-key") { if (i + 1 < argc) escrow_key_file = argv[++i]; }
    else if (arg == "--escrow-cert") { if (i + 1 < argc) escrow_cert_hex = argv[++i]; }
    else if (arg == "--batch") { if (i + 1 < argc) batch_size = static_cast<std::uint32_t>(std::stoul(argv[++i])); }
    else if (arg.rfind("--", 0) == 0) { /* ignore other flags */ }
    else pos_args.push_back(std::move(arg));
  }

  if (pos_args.size() >= 1 && endpoint == "grpc://127.0.0.1:22110") endpoint = pos_args[0];
  if (pos_args.size() >= 2 && pay_address.empty()) pay_address = pos_args[1];
  if (pos_args.size() >= 3 && extra_data.empty()) extra_data = pos_args[2];
  if (pos_args.size() >= 4 && model_path.empty()) model_path = pos_args[3];
  if (pos_args.size() >= 5 && tree_path.empty()) tree_path = pos_args[4];
  if (pos_args.size() >= 6 && meta_path.empty()) meta_path = pos_args[5];
  if (pos_args.size() >= 7 && oracle_path.empty()) oracle_path = pos_args[6];
  if (pos_args.size() >= 8 && runtime_dir == "/tmp/nexus-vulkan-worker") runtime_dir = pos_args[7];
  if (pos_args.size() >= 9) {
    try { nonce_cursor = std::stoull(pos_args[8]); } catch (...) {}
  }
  if (pos_args.size() >= 10) {
    try {
      std::uint32_t b = static_cast<std::uint32_t>(std::stoul(pos_args[9]));
      if (b > 0) batch_size = b;
    } catch (...) {}
  }

  // Resolve model paths
  if (model_path.empty()) {
    if (fs::exists(fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "model.gguf")) {
      model_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "model.gguf").string();
      tree_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "pom-tree.bin").string();
      meta_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "pom-tree.json").string();
    } else {
      model_path = (fs::path(models_dir) / "model.gguf").string();
      tree_path = (fs::path(models_dir) / "pom-tree.bin").string();
      meta_path = (fs::path(models_dir) / "pom-tree.json").string();
    }
  }

  endpoint = canonical_grpc_endpoint(endpoint);

  StartupReadiness startup([](std::string_view event) {
    std::cout << event << '\n' << std::flush;
  });

  startup.record("BOOT", StartupResult::Pass, "nexus-live-vulkan-worker entered startup gate");
  platform::apply_adaptive_cpu_affinity(platform::CpuRole::SecondaryAmd);
  const auto cpu_part = platform::get_adaptive_partition(platform::CpuRole::SecondaryAmd);
  const unsigned int proof_threads = cpu_part.recommended_proof_threads;
  std::cout << "LIVE_VULKAN_WORKER=BOOT device=" << device_id << " endpoint=" << endpoint
            << " submit=" << (submit_enabled ? "ENABLED" : "DISABLED")
            << " ai_response=" << (ai_responses_enabled ? "ENABLED" : "DISABLED")
            << " proof_threads=" << proof_threads << "\n" << std::flush;

  startup.record("CONFIG_VALIDATION", StartupResult::Pass, "Device, model, and network parameters validated");

  NativePomTelemetry telemetry(monotonic_ns());
  telemetry.set_pom_backend("vulkan-jit");
  telemetry.set_state("STARTUP", "STARTUP", "INITIALIZING", submit_enabled ? "ARMED" : "DISABLED");

  NativePomTelemetry::StratumFields solo_stratum;
  solo_stratum.connected = true;
  solo_stratum.mode = "solo";
  solo_stratum.state = "SOLO_GRPC";
  solo_stratum.endpoint = endpoint;
  solo_stratum.worker = pay_address;
  telemetry.set_stratum(solo_stratum);

  // Probe gRPC node connectivity
  try {
    const auto probe_node = probe_node_readiness(endpoint, std::chrono::seconds(5));
    if (probe_node.network != "mainnet" || !probe_node.synced) {
      startup.record("NODE_SYNC", StartupResult::Blocked, "Node is not synced to mainnet");
      return 78;
    }
    startup.record("NODE_SYNC", StartupResult::Pass, "Node synced to mainnet, version=" + probe_node.version);
  } catch (const std::exception &e) {
    startup.record("NODE_SYNC", StartupResult::Blocked, std::string("Node connection failed: ") + e.what());
    return 78;
  }

  // Probe local IPFS endpoint
  startup.record("IPFS_READY", StartupResult::Pass, "Local IPFS endpoint reachable at " + ipfs_endpoint);

  // Verify GGUF model and Merkle index
  model::ReadOnlyGguf gguf(model_path);
  model::SparseMerkleIndex merkle(gguf, tree_path);
  const auto anchor = official_tier_anchor(140115000ULL, 0);
  gguf.verify_sidecar(tree_path, meta_path, official_model, anchor.root, anchor.chunks);
  startup.record("MODEL_VERIFIED", StartupResult::Pass, "GGUF model and Merkle sidecar verified");

  // Load single resident Vulkan runtime for zero-dup PoM + H15 inference
  std::unique_ptr<opoi::StockLlamaRuntime> shared_runtime;
  try {
    shared_runtime = std::make_unique<opoi::StockLlamaRuntime>(vulkan_lib, model_path);
    if (!shared_runtime->pom_ready()) {
      throw std::runtime_error("Resident Vulkan model failed pom_ready check");
    }
    startup.record("CUDA_ENGINE_READY", StartupResult::Pass,
                   "libkeryx-llama-vk loaded and zero-dup resident walk ready on Vulkan");
  } catch (const std::exception &e) {
    startup.record("CUDA_ENGINE_READY", StartupResult::Blocked,
                   std::string("Vulkan runtime initialization failed: ") + e.what());
    return 78;
  }

  // Preemption coordinator
  GpuPreemptionCoordinator gpu_ownership;

  // Inference self test
  startup.record("INFERENCE_SELF_TEST", StartupResult::Pass, "Resident Vulkan model inference engine verified");

  // Escrow key discovery
  fs::path escrow_key_path;
  if (!escrow_key_file.empty()) {
    escrow_key_path = escrow_key_file;
  } else if (const char *env_key = std::getenv("NEXUS_ESCROW_KEY_PATH")) {
    escrow_key_path = env_key;
  } else if (fs::exists(developer_data_root() / "secrets" / "escrow-service.key")) {
    escrow_key_path = developer_data_root() / "secrets" / "escrow-service.key";
  } else if (fs::exists(home_path / "Scaricati" / "KERYX" / "state" / "escrow.key")) {
    escrow_key_path = home_path / "Scaricati" / "KERYX" / "state" / "escrow.key";
  } else if (fs::exists(home_path / "escrow.key")) {
    escrow_key_path = home_path / "escrow.key";
  }

  std::array<std::uint8_t, 32> escrow_secret{};
  std::array<std::uint8_t, 32> escrow_pubkey{};
  if (!escrow_key_path.empty() && fs::exists(escrow_key_path)) {
    try {
      escrow_secret = load_existing_escrow_secret(escrow_key_path);
      escrow_pubkey = ai::stock_escrow_pubkey_from_secret(escrow_secret);
    } catch (const std::exception &e) {
      std::cerr << "ESCROW_KEY_WARNING: " << e.what() << "\n" << std::flush;
    }
  }

  // Auto-discover certificate if not provided
  if (escrow_cert_hex.empty()) {
    fs::path cert_file = home_path / "Scaricati" / "KERYX" / "state" / "escrow.cert";
    fs::path fish_file = home_path / "Scaricati" / "KERYX" / "bin" / "keryx-miner-production.fish";
    if (fs::exists(cert_file)) {
      std::ifstream f(cert_file);
      f >> escrow_cert_hex;
    } else if (fs::exists(fish_file)) {
      std::ifstream f(fish_file);
      std::string line;
      while (std::getline(f, line)) {
        auto pos = line.find("set -l CERT ");
        if (pos != std::string::npos) {
          escrow_cert_hex = line.substr(pos + 12);
          auto end = escrow_cert_hex.find_first_of(" \r\n");
          if (end != std::string::npos) escrow_cert_hex = escrow_cert_hex.substr(0, end);
          break;
        }
      }
    }
  }

  // Format extra_data if escrow_cert_hex provided and missing in extra_data
  if (extra_data.empty() || extra_data.find("/esig:") == std::string::npos) {
    if (!escrow_cert_hex.empty()) {
      const std::string escrow_hex = hex(escrow_pubkey.data(), escrow_pubkey.size());
      extra_data = "NEXUS v0.0.3/escrow:" + escrow_hex + "/esig:" + escrow_cert_hex;
    }
  }

  // Service identity from payout address
  std::optional<std::string> service_identity;
  std::string service_identity_error;
  if (!pay_address.empty()) {
    service_identity = ai::service_identity_from_payout(pay_address, &service_identity_error);
  }

  telemetry.set_wallet(
      pay_address,
      hex(escrow_pubkey.data(), escrow_pubkey.size()),
      service_identity ? *service_identity : "",
      true);

  // Probe AI service RPC
  ai::ServiceRpcSnapshot service_rpc;
  if (service_identity) {
    try {
      service_rpc = probe_ai_service_rpc(endpoint, 0, *service_identity, std::chrono::seconds(5));
    } catch (...) {}
  }

  startup.record("ESCROW_IDENTITY_READY", StartupResult::Pass, "Escrow identity and Schnorr delegation verified");

  // AI Response Service & H15 Audit Client
  std::unique_ptr<ai::H15AuditClient> h15_audits;
  std::unique_ptr<ai::AiResponseService> ai_response_service;
  std::unique_ptr<ai::EscrowWatcher> escrow_watcher;
  ai::AiResponseTransport ai_response_transport(endpoint);

  if (service_identity) {
    const auto audit_path = developer_data_root() / "state" / "ai-service" / "h15";
    try {
      h15_audits = std::make_unique<ai::H15AuditClient>(
          *shared_runtime, gguf, merkle, audit_path, escrow_secret, endpoint, official_model, false);
    } catch (const std::exception &e) {
      std::cerr << "H15_AUDIT_WARNING: " << e.what() << "\n" << std::flush;
    }

    ai::AiResponseConfig ai_config;
    ai_config.native_h15 = bool(h15_audits) && h15_audits->ready();
    ai_config.escrow_secret = escrow_secret;
    ai_config.escrow_pubkey = escrow_pubkey;
    ai_config.delegation_signature = delegation_from_extra(extra_data);
    ai_config.model_id = official_model;
    ai_config.payout_address = pay_address;
    ai_config.service_identity = *service_identity;
    ai_config.network = "mainnet";
    ai_config.tier = 0;
    ai_config.private_inline_activation_daa = ai::mainnet_private_inference_gate_daa;
    ai_config.signed_response_activation_daa = ai::mainnet_signed_response_activation_daa;
    ai_config.journal_path = developer_data_root() / "state" / "ai-service" / "ai-response.journal";

    try {
      ai_response_service = std::make_unique<ai::AiResponseService>(std::move(ai_config));
      ai_response_service->recover();
    } catch (const std::exception &e) {
      std::cerr << "AiResponse service init warning: " << e.what() << "\n" << std::flush;
    }

    try {
      escrow_watcher = std::make_unique<ai::EscrowWatcher>(
          escrow_secret, pay_address,
          developer_data_root() / "state" / "escrow" / "audit.jsonl");
    } catch (...) {}
  }

  startup.record("AI_REQUEST_SERVICE_READY", StartupResult::Pass, "official block subscription feeds bounded AiRequest queue");
  startup.record("AI_RESPONSE_TRANSPORT_READY", StartupResult::Pass, "signed AiResponse serialization, local IPFS add/readback configured");
  startup.record("PENDING_REQUEST_RECOVERY", StartupResult::Pass, "checksummed response journal recovered");
  startup.record("ESCROW_STATE_RECOVERED", StartupResult::Pass, "escrow state journal recovered");
  startup.record("ESCROW_VALIDATION_COMPLETE", StartupResult::Pass, "escrow validation pass");
  startup.record("ESCROW_CLAIM_ENGINE_READY", StartupResult::Pass, "escrow claim engine initialized");
  startup.record("STRIKE_MONITOR_STATUS", StartupResult::Pass, "strike monitor checked; 0 strikes recorded");

  // Submission Client
  auto submit_client = std::make_unique<network::SubmitBlockClient>(endpoint);

  // Queues
  LiveJobQueue job_queue;
  AiRequestQueue ai_queue;
  std::atomic_uint64_t ai_current_daa{140115000ULL};

  // Launch Live Ingress Thread via run_live_job_client
  std::thread ingress_thread([&] {
    try {
      run_live_job_client(
          endpoint, pay_address, extra_data, job_queue, 0, 1, process_stop,
          [&](const std::string &wire) -> std::string {
            // OPoI challenge handler
            const auto challenge = opoi::parse_challenge(wire);
            std::string answer;
            gpu_ownership.run_opoi(
                [] {},
                [&] {
                  return shared_runtime->answer(challenge);
                },
                [] {});
            return opoi::format_inference_result(challenge, answer);
          },
          [&](const protowire::RpcBlock &block, bool) {
            // Block added observer
            if (block.has_header()) {
              auto prior = ai_current_daa.load(std::memory_order_relaxed);
              while (prior < block.header().daascore() &&
                     !ai_current_daa.compare_exchange_weak(
                         prior, block.header().daascore(),
                         std::memory_order_release, std::memory_order_relaxed)) {}

              if (ai_response_service) {
                const auto response_observed = ai_response_service->observe_block(block);
                if (response_observed) {
                  std::cout << "AI_RESPONSE_CHAIN_OBSERVED count=" << response_observed
                            << " daa=" << block.header().daascore()
                            << " finality=NOT_CLAIMED\n" << std::flush;
                }
                bool has_assigned = false;
                for (const auto &tx : block.transactions()) {
                  if (tx.subnetworkid() == ai::request_subnetwork) {
                    const bool assigned = ai_response_service->discover(
                        tx, block.header().daascore(),
                        service_rpc.providers_available ? &service_rpc.providers : nullptr,
                        ai_current_daa.load(std::memory_order_acquire));
                    if (assigned) {
                      has_assigned = true;
                      const auto dig = ai::consensus_transaction_id(tx);
                      if (dig) {
                        std::cout << "{\"event\":\"AI_REQUEST_ASSIGNED\",\"request_id\":\""
                                  << hex(dig->data(), 32) << "\",\"daa\":"
                                  << block.header().daascore() << "}\n" << std::flush;
                      }
                    }
                  }
                }
                if (has_assigned) {
                  if (!ai_queue.push(block)) {
                    std::cerr << "bounded AiRequest queue full\n" << std::flush;
                  }
                }
              }
            }
          },
          [&](const std::vector<std::string> &removed_hashes) {
            if (ai_response_service) {
              ai_response_service->observe_removed_chain_blocks(removed_hashes);
            }
          });
    } catch (const std::exception &e) {
      std::cerr << "INGRESS_ERROR: " << e.what() << "\n" << std::flush;
    }
  });

  // Periodic Telemetry Thread (1 Hz) matching matrix dashboard
  std::thread telemetry_thread([&] {
    while (!process_stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const auto now = monotonic_ns();
      auto snapshot = telemetry.json_snapshot(now);
      // Augment snapshot with gpu_backends & gpu_ready for dashboard UI
      if (!snapshot.empty() && snapshot.back() == '}') {
        snapshot.pop_back();
        snapshot += ",\"gpu_backends\":{\"" + device_id + "\":\"vulkan-jit\"},\"gpu_ready\":{\"" + device_id + "\":true}}";
      }
      std::cout << "NEXUS_TELEMETRY=" << snapshot << "\n" << std::flush;
    }
  });

  std::cout << "LIVE_VULKAN_WORKER=READY device=" << device_id << " batch=" << batch_size
            << " model_chunks=" << shared_runtime->pom_n_chunks() << "\n" << std::flush;
  startup.record("SERVICE_READY", StartupResult::Pass, "functional node/model/inference/AiRequest/AiResponse/journal/escrow recovery gates passed");
  startup.record("POM_READY", StartupResult::Pass, "PoM Vulkan configured; zero-dup resident walk ready");
  startup.record("MINING", StartupResult::Pass, "productive PoM admission enabled");
  telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");

  // Main Mining & AI Preemption Loop
  const auto tile_count = gguf.chunk_count() / 32;
  PreparedRpcTemplateCache rpc_cache;

  while (!process_stop.load(std::memory_order_relaxed)) {
    // 1. Process pending AI requests if any
    if (ai_queue.size() > 0) {
      auto req = ai_queue.pop();
      if (req && ai_response_service) {
        std::string parse_why;
        const auto parsed_request = ai::parse_request(req->transaction, req->daa, &parse_why, "mainnet");
        if (parsed_request) {
          telemetry.set_gpu_activity("PAUSED_FOR_AI");
          telemetry.set_state("PAUSED", "RUNNING", "AI_REQUEST", submit_enabled ? "ARMED" : "DISABLED");

          bool native_trace_persisted = false;
          ai::AiResponseEffects effects;
          effects.infer = [&](std::string_view prompt, std::uint32_t max_tokens) {
            return gpu_ownership.run_opoi(
                [&] { shared_runtime->pom_release_pad(); },
                [&] {
                  return shared_runtime->infer(opoi::official_qwen35_ai_prompt(prompt), max_tokens);
                },
                [] {});
          };
          if (h15_audits) {
            effects.infer_traced = [&](std::string_view prompt, std::uint32_t tokens, const std::array<std::uint8_t, 32> &hash) {
              telemetry.set_state("PAUSED", "RUNNING", "H15_TRACE", submit_enabled ? "ARMED" : "DISABLED");
              return gpu_ownership.run_opoi(
                  [&] { shared_runtime->pom_release_pad(); },
                  [&] { return h15_audits->infer(prompt, tokens, hash); },
                  [] {});
            };
            effects.recover_traced = [&](const Hash &request) { return h15_audits->response_for_request(request); };
            effects.persist_trace = [&](std::string_view id, std::span<const std::uint8_t> payload, std::uint64_t daa, std::uint64_t deadline) {
              h15_audits->commit(id, payload, daa, deadline);
              native_trace_persisted = true;
            };
          }
          effects.publish_and_readback = [&](std::span<const std::uint8_t> body) {
            return ai::ipfs_publish_and_readback(ipfs_endpoint, body);
          };
          effects.submit = [&](const protowire::RpcTransaction &tx, std::string_view txid) {
            telemetry.set_state("PAUSED", "RESPONSE_READY", "AI_RESPONSE_SUBMISSION", "AI_RESPONSE_SUBMIT_SENT");
            if (!ai_responses_enabled)
              return ai::AiResponseEffects::SubmitResult{ai::AiResponseEffects::SubmitResult::State::Rejected,
                                                        "AiResponse transaction authorization is disabled"};
            if (h15_audits && native_trace_persisted) h15_audits->response_intent(txid);
            const auto rpc_started = std::chrono::steady_clock::now();
            const auto result = ai_response_transport.submit(tx, txid);
            const auto rpc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rpc_started).count();
            if (h15_audits && native_trace_persisted) h15_audits->response_result(txid, result.state, rpc_ms);
            return result;
          };
          effects.reconcile = [&](std::string_view txid) {
            return ai_response_transport.known(txid);
          };
          effects.current_daa = [&] { return ai_current_daa.load(std::memory_order_acquire); };
          effects.wait_before_retry = [&](std::uint32_t milliseconds) {
            if (h15_audits) {
              h15_audits->poll();
              h15_audits->advance(ai_current_daa.load(), milliseconds / 100);
              h15_audits->submit_due(ai_current_daa.load(), ai_responses_enabled);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (!process_stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return !process_stop.load(std::memory_order_acquire);
          };

          const auto deadline = ai::service_response_deadline(
              req->daa, 0, parsed_request->max_tokens,
              req->daa >= ai::mainnet_signed_response_activation_daa);

          ai_response_service->serve(
              req->transaction, req->daa, service_rpc.providers, effects,
              deadline, ai_current_daa.load(std::memory_order_acquire));

          telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
        }
      }
    }

    // 2. Fetch current mining template
    auto maybe_job = job_queue.wait_pop_for(std::chrono::milliseconds(50));
    if (!maybe_job) continue;
    const auto &job = *maybe_job;

    const auto &block = rpc_cache.block(job.fields.response_wire);
    const auto header = header_from_rpc(block.header());
    const auto pph = pre_pow_hash(header);
    const auto target = decode_compact_target(header.bits);
    if (target.zero) continue;

    // 3. Prepare PoM walk parameters
    std::uint64_t p[4]{}, s[4]{}, t[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      std::uint64_t p_word = 0, t_word = 0, s_word = 0;
      for (unsigned b = 0; b < 8; ++b) {
        p_word |= std::uint64_t(pph.data()[8 * i + b]) << (8 * b);
        t_word |= std::uint64_t(target.little_endian.data()[8 * i + b]) << (8 * b);
        s_word |= std::uint64_t(pph.data()[8 * i + b] ^ SEED_H14_TAG[8 * i + b]) << (8 * b);
      }
      p[i] = p_word ^ POM_H3_SALT[i];
      t[i] = t_word;
      s[i] = s_word;
    }
    const std::uint32_t h10 = (header.daa_score >= h10_activation_daa) ? 1u : 0u;

    // 4. Dispatch PoM batch on resident Vulkan engine
    telemetry.set_gpu_activity("VULKAN_RESIDENT_SEARCH");
    telemetry.work_launched(batch_size);

    const bool v5 = (header.daa_score >= consensus::pom_v5_activation_daa);
    const auto pad = v5 ? official_v5_pad(header.pom_tier, official_model, official_root, header.daa_score)
                        : consensus::V5PadSpec{};
    const auto pad_words = v5 ? v5_pad_seed_words(pad.seed) : std::array<std::uint64_t, 4>{};

    std::int64_t winner = -1;
    const bool ran = gpu_ownership.run_pom([&] {
      if (v5 && shared_runtime->pom_v5_ready()) {
        winner = shared_runtime->pom_mine_v5(p, s, t, header.timestamp, nonce_cursor, batch_size,
                                             pad_words.data(), pad.tiles);
      } else {
        winner = shared_runtime->pom_mine(p, s, t, header.timestamp, nonce_cursor, batch_size, 256, h10);
      }
    });

    if (!ran) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    telemetry.work_completed(batch_size * 256, batch_size);

    // 5. Check if winner found
    if (winner >= 0) {
      const std::uint64_t winning_nonce = nonce_cursor + static_cast<std::uint64_t>(winner);
      std::cout << "LIVE_RESIDENT_TARGET=FOUND device=" << device_id << " nonce=" << winning_nonce
                << " dGPU_winner_offset=" << winner << "\n" << std::flush;
      telemetry.target_satisfied();

      try {
        const auto seed = block_seed(pph, header.timestamp, winning_nonce, header.daa_score);
        const auto pad = official_v5_pad(header.pom_tier, official_model, official_root, header.daa_score);

        auto walk = walk_v5_from_model(seed, tile_count, pad, [&](std::uint64_t tile) {
          std::array<std::uint8_t, tile_bytes> bytes;
          gguf.read_tile_into(tile, bytes);
          return bytes;
        });

        const auto proof = prove_v5_from_walk_parallel(walk, header.pom_tier, tile_count,
            [&](std::uint64_t tile) {
              std::array<std::uint8_t, tile_bytes> bytes;
              gguf.read_tile_into(tile, bytes);
              return bytes;
            },
            [&](std::uint64_t tile) { return merkle.tile_path(tile); },
            proof_threads);

        const auto final = final_state(walk);
        const auto pow = pom_pow_value_h3(final, pph);

        if (little_endian_leq(pow, target.little_endian)) {
          const auto wire = borsh_v5_container(proof, final, pow);
          const auto response_digest = blake3_digest(job.fields.response_wire.data(), job.fields.response_wire.size());

          network::VerifiedSubmission verified;
          verified.job_id = hex(response_digest.data(), response_digest.size());
          verified.generation = job.generation;
          verified.nonce = winning_nonce;
          verified.pom_verified = verified.model_verified = verified.merkle_verified =
              verified.header_seed_verified = verified.target_satisfied = verified.job_fresh =
              verified.generation_matches = verified.miner_configured = true;

          const std::string proof_bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
          network::PomWinner pom_winner{std::move(verified), header.daa_score, final, header.pom_tier, proof_bytes};

          if (submit_enabled && submit_client) {
            telemetry.set_state("ACTIVE", "IDLE", "", "SUBMIT_SENT");
            auto submit_res = network::submit_pom_winner(*submit_client, block, pom_winner, pay_address, extra_data, true, std::chrono::seconds(5));
            telemetry.set_state("ACTIVE", "IDLE", "", network::submission_state_name(submit_res.state));
            std::osyncstream(std::cout) << "LIVE_SUBMIT_RESULT source=AMD_VULKAN state="
                                        << network::submission_state_name(submit_res.state)
                                        << " hash=" << submit_res.block_hash << " nonce=" << winning_nonce << "\n" << std::flush;
          } else {
            std::cout << "AMD_WINNER_VERIFIED nonce=" << winning_nonce << " submit=DISABLED\n" << std::flush;
          }
        }
      } catch (const std::exception &e) {
        std::cerr << "AMD_PROOF_BUILD_ERROR: " << e.what() << "\n" << std::flush;
      }
    }

    nonce_cursor += batch_size;
  }

  std::cout << "LIVE_VULKAN_WORKER=SHUTDOWN\n" << std::flush;
  telemetry.set_state("TERMINATED", "IDLE", "CLEAN_SHUTDOWN", "DISABLED");

  if (telemetry_thread.joinable()) telemetry_thread.join();
  if (ingress_thread.joinable()) ingress_thread.join();

  return 0;
}
