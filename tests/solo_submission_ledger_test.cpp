#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "keryx/nexus/submission_ledger.hpp"
#include "messages.grpc.pb.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <map>
#include <mutex>
using namespace keryx::nexus;
using namespace keryx::nexus::network;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
std::string hex(const consensus::Hash &h) {
  std::string s;
  for (auto c : h) {
    s += "0123456789abcdef"[c >> 4];
    s += "0123456789abcdef"[c & 15];
  }
  return s;
}
consensus::Hash block_id(const protowire::RpcBlock &b) {
  return consensus::block_hash_for_daa(
      consensus::header_from_rpc(b.header()), b.header().nonce(),
      static_cast<std::uint64_t>(b.header().timestamp()));
}
protowire::RpcBlock candidate(unsigned nonce) {
  protowire::RpcBlock b;
  auto *h = b.mutable_header();
  h->set_version(1);
  h->set_timestamp(1700000123);
  h->set_daascore(148950000);
  h->set_bits(0x207fffff);
  h->set_hashmerkleroot(std::string(64, 'a'));
  h->set_acceptedidmerkleroot(std::string(64, 'b'));
  h->set_utxocommitment(std::string(64, 'c'));
  h->set_pruningpoint(std::string(64, 'd'));
  h->set_bluework("01");
  h->set_pomtier(0);
  h->add_parents()->add_parenthashes(std::string(64, 'e'));
  h->set_nonce(nonce);
  b.set_pomproof("00");
  return b;
}
class Node final : public protowire::RPC::Service {
  std::mutex mutex_;
  std::map<std::string, protowire::RpcBlock> blocks_;

public:
  std::atomic_int mode{};
  std::atomic_uint submits{};
  std::atomic_bool hide_blocks{};
  grpc::Status MessageStream(
      grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse,
                               protowire::KaspadRequest> *stream) override {
    protowire::KaspadRequest q;
    while (stream->Read(&q)) {
      protowire::KaspadResponse r;
      r.set_id(q.id());
      if (q.has_submitblockrequest()) {
        ++submits;
        const auto &b = q.submitblockrequest().block();
        {
          std::lock_guard l(mutex_);
          blocks_[hex(block_id(b))] = b;
        }
        if (mode == 1)
          return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              "synthetic ACK loss");
        if (mode == 2)
          r.set_id(q.id() + 1);
        auto *reply = r.mutable_submitblockresponse();
        if (mode == 4)
          reply->mutable_error();
        if (mode == 3) {
          reply->set_rejectreason(
              protowire::SubmitBlockResponseMessage::BLOCK_INVALID);
          reply->mutable_error()->set_message("synthetic rejection");
        }
      } else if (q.has_getblockrequest()) {
        const auto &hash = q.getblockrequest().hash();
        auto *reply = r.mutable_getblockresponse();
        std::lock_guard l(mutex_);
        auto at = blocks_.find(hash);
        if (hide_blocks ||
            (at == blocks_.end() && hash != std::string(64, 'f')))
          reply->mutable_error()->set_message("not observed");
        else {
          auto *b = reply->mutable_block();
          if (at != blocks_.end())
            *b = at->second;
          else
            *b = candidate(0);
          b->mutable_verbosedata()->set_hash(hash);
          b->mutable_verbosedata()->set_ischainblock(true);
          b->mutable_verbosedata()->set_bluescore(
              hash == std::string(64, 'f') ? 110 : 100);
        }
      } else if (q.has_getblockdaginforequest())
        r.mutable_getblockdaginforesponse()->set_sink(std::string(64, 'f'));
      else
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            "synthetic RPC only");
      if (!stream->Write(r))
        break;
    }
    return grpc::Status::OK;
  }
};
int main(int argc, char **argv) {
  try {
    check(argc == 2, "usage: solo-submission-ledger-test persistent-root");
    auto root = std::filesystem::absolute(argv[1]);
    check(!root.string().starts_with("/tmp/"), "persistent root required");
    root /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    auto path = root / "solo.journal";
    Node node;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&node);
    auto server = builder.BuildAndStart();
    check(bool(server), "mock node start failed");
    const auto endpoint = "grpc://127.0.0.1:" + std::to_string(port);
    for (int mode = 0; mode < 5; ++mode) {
      node.mode = mode;
      const auto b = candidate(static_cast<unsigned>(mode + 7));
      const auto id = block_id(b);
      VerifiedSubmission verified{"synthetic", 1,    b.header().nonce(),
                                  true,        true, true,
                                  true,        true, true,
                                  true,        true};
      const auto before = node.submits.load();
      {
        SubmitBlockClient client(endpoint, path);
        auto result = client.submit(b, verified, true);
        check(result.timing.journal_intent_ms.has_value() &&
                  result.timing.journal_sent_ms.has_value(),
              "durability latency spans missing");
        if (mode == 0 || mode == 3 || mode == 4)
          check(result.timing.journal_receipt_ms.has_value(),
                "correlated receipt durability latency missing");
        const auto expected = mode == 0 ? SubmissionState::RpcAcknowledged
                              : (mode == 3 || mode == 4)
                                  ? SubmissionState::RpcRejected
                                  : SubmissionState::UnknownOutcome;
        check(result.state == expected, "transport receipt misclassified");
      }
      check(node.submits == before + 1, "unexpected first wire count");
      {
        SubmissionLedger ledger(path);
        const auto expected = mode == 0 ? SubmissionLedger::State::Acknowledged
                              : (mode == 3 || mode == 4)
                                  ? SubmissionLedger::State::Rejected
                                  : SubmissionLedger::State::Unknown;
        check(ledger.lookup(id) == expected, "transport state not durable");
      }
      {
        SubmitBlockClient restarted(endpoint, path);
        verified.job_id = "different-session-job";
        const auto result = restarted.submit(b, verified, true);
        check(result.state == SubmissionState::UnknownOutcome,
              "restart fabricated a new transport ACK");
        check(node.submits == before + 1, "restart duplicated SubmitBlock");
        if (mode == 1) {
          node.hide_blocks = true;
          auto missing =
              restarted.observe_chain(result, std::chrono::seconds(1),
                                      std::chrono::milliseconds(10), 6);
          check(missing.state == SubmissionState::UnknownOutcome,
                "absence fabricated success");
          node.hide_blocks = false;
        }
        // A canonical read can resolve chain presence, without inventing a lost
        // ACK.
        const auto observed = restarted.observe_chain(
            result, std::chrono::seconds(1), std::chrono::milliseconds(10), 6);
        check(observed.state == SubmissionState::ChainConfirmed,
              "read-only chain reconciliation failed");
        check(std::find(observed.transitions.begin(),
                        observed.transitions.end(),
                        SubmissionState::RpcAcknowledged) ==
                  observed.transitions.end(),
              "chain observation fabricated RPC ACK history");
      }
      {
        SubmissionLedger ledger(path);
        check(ledger.lookup(id) == SubmissionLedger::State::ChainObserved,
              "chain evidence not persisted distinctly");
      }
    }
    {
      SubmitBlockClient client(endpoint, root / "gates.journal");
      auto b = candidate(50);
      VerifiedSubmission verified{"gate", 1,    50,   true, true, true,
                                  true,   true, true, true, true};
      const auto before = node.submits.load();
      bool failed = false;
      try {
        client.submit(b, verified, false);
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed && node.submits == before,
            "disabled submission crossed wire");
      verified.pom_verified = false;
      failed = false;
      try {
        client.submit(b, verified, true);
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed && node.submits == before, "unverified proof crossed wire");
    }
    auto broken = root / "broken.journal";
    {
      SubmitBlockClient client(endpoint, broken);
      {
        std::ofstream f(broken, std::ios::app | std::ios::binary);
        f.put('x');
      }
      auto b = candidate(51);
      VerifiedSubmission v{"disk", 1,    51,   true, true, true,
                           true,   true, true, true, true};
      auto before = node.submits.load();
      bool failed = false;
      try {
        client.submit(b, v, true);
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed && node.submits == before,
            "corrupt ledger authorized SubmitBlock");
    }
    server->Shutdown();
    std::cout << "SOLO_SUBMISSION_LEDGER=PASS restart_no_resend=true "
                 "ack_loss_unknown=true wrong_rpc_id_unknown=true "
                 "empty_rpc_error_rejected=true "
                 "chain_observed_not_ack=true durable_receipts=true "
                 "disk_failure_no_send=true proof=synthetic_transport_only\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
