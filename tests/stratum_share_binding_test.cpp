#include "keryx/nexus/stratum_v4_client.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/struct.pb.h>
#include <stdexcept>
using namespace keryx::nexus::stratum;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
std::string hex(std::string_view b) {
  std::string r;
  for (unsigned char c : b) {
    r += "0123456789abcdef"[c >> 4];
    r += "0123456789abcdef"[c & 15];
  }
  return r;
}
int main(int argc, char **argv) {
  try {
    StratumClientConfig cfg;
    cfg.pool_url = "stratum+tcp://127.0.0.1:1";
    cfg.worker = "synthetic";
    StratumV4Client c(cfg);
    c.enable_loopback_test_mode(true);
    protowire::RpcBlockHeader h;
    h.set_version(1);
    h.set_timestamp(1700000123);
    h.set_daascore(148950000);
    h.set_bits(0x01010000);
    h.set_hashmerkleroot(std::string(64, 'a'));
    h.set_acceptedidmerkleroot(std::string(64, 'b'));
    h.set_utxocommitment(std::string(64, 'c'));
    h.set_pruningpoint(std::string(64, 'd'));
    h.set_bluework("01");
    h.set_pomtier(0);
    h.add_parents()->add_parenthashes(std::string(64, 'e'));
    h.set_taskroot(std::string(64, 'f'));
    {
      StratumV4Client rejected(cfg);
      rejected.enable_loopback_test_mode(true);
      check(rejected.connect_and_handshake_once(), "auth fixture handshake failed");
      const auto frames = rejected.take_outbound_wire_log();
      check(frames.size() == 2, "auth fixture frame count");
      google::protobuf::Struct auth;
      check(google::protobuf::util::JsonStringToMessage(frames[1], &auth).ok(), "auth JSON");
      const auto auth_id = static_cast<std::uint64_t>(auth.fields().at("id").number_value());
      check(rejected.process_inbound_line("{\"id\":" + std::to_string(auth_id) +
          ",\"result\":null,\"error\":[24,\"H16 compatibility gate: synthetic miner not approved\",null]}"), "auth rejection failed");
      check(rejected.process_inbound_line(
          "{\"method\":\"mining.notify\",\"params\":[\"rejected-job\",\"" + hex(h.SerializeAsString()) +
          "\",\"1700000123\",\"148950000\",16842752,\"\"]}"), "post-rejection notify failed");
      check(rejected.process_inbound_line("{\"method\":\"mining.set_difficulty\",\"params\":[2]}"), "post-rejection difficulty failed");
      const auto status = rejected.telemetry_snapshot();
      check(!status.connected && status.state == "AUTH_REJECTED", "notify erased authorization rejection");
      check(status.last_error.find("code 24") != std::string::npos &&
          status.last_error.find("H16 compatibility gate") != std::string::npos,
          "pool authorization error detail lost");
      check(status.pool_telemetry_state == "AUTH_REJECTED" && status.rejected_shares == 0,
          "authorization rejection changed share receipts");
      rejected.poll_pool_extensions();
      check(rejected.take_outbound_wire_log().empty(), "rejected session sent pool capabilities");
      rejected.enable_loopback_test_mode(false);
      check(!rejected.current_job(), "rejected session offered mining work");
      check(rejected.process_inbound_line("{\"id\":" + std::to_string(auth_id) +
          ",\"result\":true,\"error\":null}"), "positive authorization failed");
      check(rejected.telemetry_snapshot().connected && rejected.telemetry_snapshot().last_error.empty(),
          "positive authorization did not recover rejection");
    }
    check(c.process_inbound_line(
              "{\"method\":\"mining.set_difficulty\",\"params\":[1]}"),
          "initial difficulty failed");
    check(c.process_inbound_line(
              "{\"method\":\"mining.notify\",\"params\":[\"synthetic-job\",\"" +
              hex(h.SerializeAsString()) +
              "\",\"1700000123\",\"148950000\",16842752,\"\"]}"),
          "notify failed");
    auto job = c.current_job();
    check(job.has_value(), "job missing");
    VerifiedPoolShare share;
    share.job_id = job->job_id;
    share.job_generation = job->generation;
    share.protocol_version = job->protocol_version;
    share.nonce = 7;
    share.timestamp = job->timestamp;
    share.daa_score = job->daa_score;
    share.pre_pow = job->pre_pow;
    share.effective_target = job->effective_target;
    share.pow_value = job->effective_target;
    share.network_target = job->network_target;
    share.has_task_root = job->has_task_root;
    share.task_root = job->task_root;
    share.pom_proof_borsh.resize(73);
    check(c.process_inbound_line(
              "{\"method\":\"mining.set_difficulty\",\"params\":[1000000]}"),
          "retarget failed");
    check(c.current_job()->generation == share.job_generation,
          "retarget reset the nonce generation");
    check(c.current_job()->effective_target == job->effective_target &&
              c.current_job()->difficulty == 1,
          "pending difficulty rewrote the active job target");
    check(c.submit_share(share).has_value(),
          "valid bound-job share lost to a pending harder difficulty");
    check(c.take_outbound_wire_log().size() == 1,
          "bound-job share did not write once");
    check(c.process_inbound_line(
              "{\"method\":\"mining.notify\",\"params\":[\"next-hard-job\",\"" +
              hex(h.SerializeAsString()) +
              "\",\"1700000123\",\"148950000\",16842752,\"\"]}"),
          "next hard notify failed");
    auto hard = c.current_job();
    check(hard && hard->effective_target != job->effective_target &&
              hard->difficulty == 1000000,
          "next notify did not bind pending difficulty");
    share.job_id = hard->job_id;
    share.job_generation = hard->generation;
    share.nonce = 8;
    // A candidate carrying the previous threshold cannot cross a harder job
    // binding.
    check(!c.submit_share(share),
          "candidate with stale target snapshot transmitted");
    check(c.take_outbound_wire_log().empty(), "target binding drop wrote wire");
    share.pow_value = {};
    share.effective_target = hard->effective_target;
    check(c.submit_share(share).has_value(),
          "valid replacement suppressed by dedup");
    check(c.take_outbound_wire_log().size() == 1,
          "expected one replacement wire send");
    check(!c.submit_share(share), "duplicate share was resent");
    check(c.take_outbound_wire_log().empty(), "duplicate produced wire send");
    // An invalid proof against an unchanged target remains a hard error.
    auto bad = share;
    bad.nonce = 9;
    bad.pow_value.fill(0xff);
    bool rejected = false;
    try {
      (void)c.submit_share(bad);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    check(rejected, "invalid local proof hidden as retarget staleness");
    bad.pow_value = {};
    check(c.submit_share(bad).has_value(),
          "validation failure poisoned nonce dedup");
    (void)c.take_outbound_wire_log();
    check(c.process_inbound_line(
              "{\"method\":\"mining.set_difficulty\",\"params\":[1]}"),
          "easier update failed");
    check(c.current_job()->effective_target == hard->effective_target,
          "easier pending target leaked into old job");
    auto weak = share;
    weak.nonce = 10;
    weak.effective_target = job->effective_target;
    weak.pow_value = job->effective_target;
    check(!c.submit_share(weak) && c.take_outbound_wire_log().empty(),
          "weak share crossed the old job after easier vardiff update");
    check(c.process_inbound_line(
              "{\"method\":\"mining.notify\",\"params\":[\"next-easy-job\",\"" +
              hex(h.SerializeAsString()) +
              "\",\"1700000123\",\"148950000\",16842752,\"\"]}"),
          "next easier notify failed");
    weak.job_id = c.current_job()->job_id;
    weak.job_generation = c.current_job()->generation;
    check(c.current_job()->effective_target == job->effective_target &&
              c.submit_share(weak).has_value(),
          "easier difficulty did not activate with its own notify");
    check(c.take_outbound_wire_log().size() == 1,
          "easier next-job share wire count");
    check(argc == 2, "usage: stratum-share-binding-test persistent-root");
    auto root = std::filesystem::absolute(argv[1]);
    check(!root.string().starts_with("/tmp/"), "persistent test root required");
    root /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    auto durable_cfg = cfg;
    durable_cfg.submission_journal = root / "pool-shares.journal";
    auto configured = [&](StratumV4Client &client, std::string_view id) {
      client.enable_loopback_test_mode(true);
      check(client.process_inbound_line(
                "{\"method\":\"mining.set_difficulty\",\"params\":[1]}"),
            "durable difficulty setup");
      check(client.process_inbound_line(
                "{\"method\":\"mining.notify\",\"params\":[\"" +
                std::string(id) + "\",\"" + hex(h.SerializeAsString()) +
                "\",\"1700000123\",\"148950000\",16842752,\"\"]}"),
            "durable notify setup");
      const auto job = client.current_job();
      auto result = share;
      result.job_id = job->job_id;
      result.job_generation = job->generation;
      result.effective_target = job->effective_target;
      result.pow_value = {};
      return result;
    };
    // Exact ACK loss: drop the client after a wire write, recreate the same
    // work with a different pool job ID. No replay and no fabricated ACK.
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "before-reconnect");
      item.nonce = 11;
      check(a.submit_share(item).has_value(), "durable first send failed");
      check(a.take_outbound_wire_log().size() == 1, "first durable wire count");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "after-reconnect");
      item.nonce = 11;
      check(!a.submit_share(item),
            "restart resent ambiguous work under new job ID");
      check(a.take_outbound_wire_log().empty() &&
                a.telemetry_snapshot().accepted_shares == 0,
            "lost ACK fabricated send/success");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "accepted");
      item.nonce = 12;
      const auto id = a.submit_share(item);
      check(id.has_value(), "ACK fixture send failed");
      check(a.process_inbound_line("{\"id\":" + std::to_string(*id) +
                                   ",\"result\":true,\"error\":null}"),
            "ACK fixture reply failed");
      check(a.telemetry_snapshot().accepted_shares == 1, "real ACK lost");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "accepted-restarted");
      item.nonce = 12;
      check(!a.submit_share(item), "ACKed work resent after restart");
      check(a.take_outbound_wire_log().empty(), "ACKed duplicate wrote wire");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "malformed-receipt");
      item.nonce = 13;
      const auto id = a.submit_share(item);
      check(id.has_value(), "malformed fixture send failed");
      check(a.process_inbound_line("{\"id\":" + std::to_string(*id) +
                                   ",\"result\":null,\"error\":null}"),
            "malformed fixture reply parser failed");
      auto stats = a.telemetry_snapshot();
      check(stats.accepted_shares == 0 && stats.rejected_shares == 0 &&
                stats.pending_shares == 0,
            "malformed receipt invented ACK/rejection");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "malformed-restarted");
      item.nonce = 13;
      check(!a.submit_share(item), "malformed-receipt work resent");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "explicit-rejection");
      item.nonce = 14;
      const auto id = a.submit_share(item);
      check(id.has_value(), "rejection fixture send failed");
      check(
          a.process_inbound_line(
              "{\"id\":" + std::to_string(*id) +
              ",\"result\":null,\"error\":[23,\"synthetic rejection\",null]}"),
          "rejection parser failed");
      check(a.telemetry_snapshot().rejected_shares == 1,
            "explicit rejection lost");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "rejected-restarted");
      item.nonce = 14;
      check(!a.submit_share(item), "rejected work resent");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "failed-write");
      item.nonce = 15;
      a.enable_loopback_test_mode(false);
      check(!a.submit_share(item), "missing socket authorized a send");
    }
    {
      StratumV4Client a(durable_cfg);
      auto item = configured(a, "failed-write-restarted");
      item.nonce = 15;
      check(!a.submit_share(item) && a.take_outbound_wire_log().empty(),
            "ambiguous failed write resent after restart");
    }
    auto broken_cfg = cfg;
    broken_cfg.submission_journal = root / "disk-fault.journal";
    {
      StratumV4Client a(broken_cfg);
      auto item = configured(a, "disk-fault");
      item.nonce = 16;
      {
        std::ofstream f(broken_cfg.submission_journal,
                        std::ios::app | std::ios::binary);
        f.put('x');
      }
      bool failed = false;
      try {
        (void)a.submit_share(item);
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed && a.take_outbound_wire_log().empty(),
            "corrupt ledger authorized network send");
    }
    std::cout << "STRATUM_SHARE_BINDING=PASS notify_bound_difficulty=true "
                 "pending_harder_preserves_job=true "
                 "pending_easier_no_weak_share=true "
                 "valid_same_nonce_after_drop=true duplicate_suppressed=true "
                 "durable_restart=true ack_loss_unknown=true "
                 "disk_failure_no_send=true proof=synthetic_transport_only\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
