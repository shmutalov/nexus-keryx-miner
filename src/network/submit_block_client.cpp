#include "keryx/nexus/submission_ledger.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "messages.grpc.pb.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace keryx::nexus::network {
namespace {
std::pair<std::string, bool> endpoint_target(const std::string &endpoint) {
  if (endpoint.rfind("grpcs://", 0) == 0) return {endpoint.substr(8), true};
  if (endpoint.rfind("grpc://", 0) != 0)
    throw std::invalid_argument("SubmitBlock endpoint must use grpcs:// or loopback-only grpc://");
  const auto target = endpoint.substr(7);
  std::string host;
  if (!target.empty() && target.front() == '[') {
    const auto end = target.find(']');
    if (end == std::string::npos || end + 1 >= target.size() || target[end + 1] != ':')
      throw std::invalid_argument("malformed IPv6 SubmitBlock endpoint");
    host = target.substr(1, end - 1);
  } else {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument("SubmitBlock endpoint has no port");
    host = target.substr(0, colon);
  }
  in_addr v4{};
  in6_addr v6{};
  const bool loop4 = inet_pton(AF_INET, host.c_str(), &v4) == 1 &&
                     (ntohl(v4.s_addr) >> 24U) == 127U;
  const bool loop6 = inet_pton(AF_INET6, host.c_str(), &v6) == 1 && IN6_IS_ADDR_LOOPBACK(&v6);
  if (!loop4 && !loop6)
    throw std::invalid_argument("plaintext SubmitBlock is restricted to numeric loopback");
  return {target, false};
}

std::shared_ptr<grpc::Channel> make_channel(const std::string &endpoint) {
  const auto [target, tls] = endpoint_target(endpoint);
  return grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
}

std::shared_ptr<grpc::Channel> connect(const std::string &endpoint,
                                       std::chrono::system_clock::time_point deadline,
                                       std::shared_ptr<grpc::Channel> channel = nullptr) {
  if (!channel) channel = make_channel(endpoint);
  if (!channel->WaitForConnected(deadline))
    throw std::runtime_error("node RPC channel connection timed out before request was sent");
  return channel;
}

protowire::KaspadResponse exchange(const std::string &endpoint,
                                  const protowire::KaspadRequest &request,
                                  std::chrono::system_clock::time_point deadline,
                                  std::shared_ptr<grpc::Channel> reusable_channel = nullptr) {
  auto channel = connect(endpoint, deadline, std::move(reusable_channel));
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) {
    throw std::runtime_error("could not create node RPC.MessageStream");
  }
  if (!stream->Write(request) || !stream->WritesDone()) {
    throw std::runtime_error("node RPC request write failed");
  }
  protowire::KaspadResponse response;
  if (!stream->Read(&response) || response.id() != request.id()) {
    throw std::runtime_error("node RPC response missing or correlation ID mismatch");
  }
  const auto status = stream->Finish();
  if (!status.ok()) {
    throw std::runtime_error("node RPC failed: " + status.error_message());
  }
  return response;
}

std::string hash_hex(const consensus::Hash &hash) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(hash.size() * 2, '0');
  for (std::size_t i = 0; i < hash.size(); ++i) {
    const auto value = hash[i];
    result[i * 2] = digits[value >> 4];
    result[i * 2 + 1] = digits[value & 0x0f];
  }
  return result;
}

struct BlockObservation {
  bool found{};
  bool selected_chain{};
  std::uint64_t blue_score{};
  std::uint64_t selected_tip_blue_score{};
};

bool fetch_block(const std::string &endpoint, const std::string &hash,
                 protowire::RpcBlock &block,
                 std::chrono::system_clock::time_point deadline,
                 const std::shared_ptr<grpc::Channel> &channel = nullptr) {
  protowire::KaspadRequest request;
  request.set_id(1);
  request.mutable_getblockrequest()->set_hash(hash);
  request.mutable_getblockrequest()->set_includetransactions(false);
  const auto response = exchange(endpoint, std::move(request), deadline, channel);
  if (!response.has_getblockresponse())
    throw std::runtime_error("GetBlock response payload missing");
  const auto &payload = response.getblockresponse();
  if (payload.has_error()) return false;
  if (!payload.has_block() || !payload.block().has_header() ||
      !payload.block().has_verbosedata())
    throw std::runtime_error("GetBlock returned no full block/verbose status");
  const auto &verbose = payload.block().verbosedata();
  std::string returned_hash = verbose.hash();
  std::string requested_hash = hash;
  std::transform(returned_hash.begin(), returned_hash.end(), returned_hash.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(requested_hash.begin(), requested_hash.end(), requested_hash.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (returned_hash != requested_hash)
    throw std::runtime_error("GetBlock returned a different block hash");
  block.CopyFrom(payload.block());
  return true;
}

BlockObservation observe_once(const std::string &endpoint, const std::string &candidate_hash,
                              std::chrono::system_clock::time_point deadline,
                              const std::shared_ptr<grpc::Channel> &channel = nullptr) {
  BlockObservation observation;
  protowire::RpcBlock candidate;
  if (!fetch_block(endpoint, candidate_hash, candidate, deadline, channel)) return observation;
  observation.found = true;
  observation.selected_chain = candidate.verbosedata().ischainblock();
  observation.blue_score = candidate.verbosedata().bluescore();

  protowire::KaspadRequest dag_request;
  dag_request.set_id(2);
  dag_request.mutable_getblockdaginforequest();
  const auto dag_response = exchange(endpoint, std::move(dag_request), deadline, channel);
  if (!dag_response.has_getblockdaginforesponse())
    throw std::runtime_error("GetBlockDagInfo response payload missing");
  const auto &dag = dag_response.getblockdaginforesponse();
  if (dag.has_error() || dag.sink().empty())
    throw std::runtime_error("GetBlockDagInfo did not provide the selected sink");
  protowire::RpcBlock sink;
  if (!fetch_block(endpoint, dag.sink(), sink, deadline, channel))
    throw std::runtime_error("selected sink was not available through GetBlock");
  observation.selected_tip_blue_score = sink.verbosedata().bluescore();
  return observation;
}

void transition(SubmitBlockResult &result, SubmissionState state) {
  result.state = state;
  if (result.transitions.empty() || result.transitions.back() != state)
    result.transitions.push_back(state);
}
}  // namespace

const char *submission_state_name(SubmissionState state) noexcept {
  switch (state) {
    case SubmissionState::CandidateValid: return "CANDIDATE_VALID";
    case SubmissionState::SubmitSent: return "SUBMIT_SENT";
    case SubmissionState::RpcAcknowledged: return "RPC_ACKNOWLEDGED";
    case SubmissionState::RpcRejected: return "RPC_REJECTED";
    case SubmissionState::ChainObserved: return "CHAIN_OBSERVED";
    case SubmissionState::ChainConfirmed: return "CHAIN_CONFIRMED";
    case SubmissionState::UnknownAfterTimeout: return "UNKNOWN_AFTER_TIMEOUT";
    case SubmissionState::UnknownOutcome: return "UNKNOWN_OUTCOME";
  }
  return "UNKNOWN_OUTCOME";
}

struct SubmitBlockClient::PersistentStream {
  grpc::ClientContext context;
  std::unique_ptr<grpc::ClientReaderWriter<protowire::KaspadRequest,protowire::KaspadResponse>> wire;
  ~PersistentStream() {
    context.TryCancel();
    if(wire)(void)wire->Finish();
  }
};
SubmitBlockClient::~SubmitBlockClient() = default;

SubmitBlockClient::SubmitBlockClient(std::string endpoint,std::filesystem::path journal) : endpoint_(std::move(endpoint)) {
  (void)endpoint_target(endpoint_); // validate transport policy before any request can be made
  if(!journal.empty())submission_ledger_=std::make_unique<SubmissionLedger>(std::move(journal));
}

SubmitBlockResult SubmitBlockClient::submit(const protowire::RpcBlock &block,
                                             const VerifiedSubmission &verified,
                                             bool enable_live_submit,
                                             std::chrono::seconds timeout,
                                             const std::function<bool()> &cancelled) {
  std::unique_lock stream_lock(stream_mutex_); // one correlated request in flight
  const auto begin=std::chrono::steady_clock::now();
  auto phase=begin;
  const auto elapsed=[](auto since){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-since).count();};
  if (!enable_live_submit)
    throw std::invalid_argument("live SubmitBlock is disabled; explicit enable flag required");
  if (timeout <= std::chrono::seconds::zero())
    throw std::invalid_argument("SubmitBlock timeout must be positive");
  if (!block.has_header() || block.pomproof().empty())
    throw std::invalid_argument("SubmitBlock candidate must contain header and PoM proof bytes");
  if (block.header().nonce() != verified.nonce)
    throw std::invalid_argument("SubmitBlock nonce does not match verified candidate");

  SubmitBlockResult output;
  if(cancelled && cancelled())return output;
  transition(output, SubmissionState::CandidateValid);
  output.reservation = gate_.reserve(verified);
  const auto candidate_header = consensus::header_from_rpc(block.header());
  const auto ledger_id=consensus::block_hash_for_daa(candidate_header,block.header().nonce(),
      static_cast<std::uint64_t>(block.header().timestamp()));
  output.block_hash=hash_hex(ledger_id);
  output.timing.validation_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
  const auto done=[&]{output.timing.total_ms=elapsed(begin);return output;};
  if(submission_ledger_) {
    const auto journal_started=std::chrono::steady_clock::now();
    const auto bytes=block.SerializeAsString();
    const auto digest=consensus::blake3_hash(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
    const bool reserved=submission_ledger_->reserve(ledger_id,digest);
    output.timing.journal_intent_ms=elapsed(journal_started);
    if(!reserved) {
      // Preserve ambiguity after restart and permit read-only chain observation.
      // A prior wire attempt, even a rejected one, never authorizes a blind replay.
      output.node_error="DURABLE_INTENT_ALREADY_EXISTS; no retransmission";
      transition(output,SubmissionState::UnknownOutcome);return done();
    }
  }
  phase=std::chrono::steady_clock::now(); // Journal time is separate from connection time.
  const auto deadline = std::chrono::system_clock::now() + timeout;
  std::shared_ptr<grpc::Channel> channel;
  try {
    {
    std::lock_guard lock(submit_channel_mutex_);
    output.timing.channel_reused=static_cast<bool>(submit_channel_);
    if(!submit_channel_) {
      const auto [target,tls]=endpoint_target(endpoint_);
      submit_channel_=grpc::CreateChannel(target,tls?grpc::SslCredentials(grpc::SslCredentialsOptions{}):grpc::InsecureChannelCredentials());
    }
    channel=submit_channel_;
    }
    while(channel->GetState(true)!=GRPC_CHANNEL_READY) {
      if(cancelled && cancelled())return done();
      if(std::chrono::system_clock::now()>=deadline)throw std::runtime_error("node RPC channel connection timed out before request was sent");
      channel->WaitForStateChange(channel->GetState(false),
          std::min(deadline,std::chrono::system_clock::now()+std::chrono::milliseconds(10)));
    }
  } catch (...) {
    // No request was written; preserve the dedupe reservation for safety but
    // report that the candidate remains valid rather than an unknown submit.
    throw;
  }
  output.timing.connect_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
  output.timing.stream_reused=static_cast<bool>(stream_);
  if(!stream_) {
    stream_=std::make_unique<PersistentStream>();
    auto stub=protowire::RPC::NewStub(channel);
    stream_->wire=stub->MessageStream(&stream_->context);
    if(!stream_->wire) {stream_.reset();throw std::runtime_error("could not create persistent SubmitBlock stream");}
  }
  auto *active=stream_.get();
  std::atomic_bool timed_out{false};
  // A persistent context cannot carry a per-request gRPC deadline. Cancel the
  // whole stream on timeout/preemption; reconnect only for a NEW candidate.
  std::jthread cancellation([&](std::stop_token stop){
    while(!stop.stop_requested()) {
      if(std::chrono::system_clock::now()>=deadline) {
        timed_out.store(true);active->context.TryCancel();return;
      }
      try {if(cancelled && cancelled()){active->context.TryCancel();return;}}
      catch(...){active->context.TryCancel();return;}
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  const auto discard_stream=[&]{
    cancellation.request_stop();cancellation.join();
    stream_.reset();
  };
  output.timing.stream_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
  protowire::KaspadRequest request;
  if(next_request_id_==0) {discard_stream();throw std::overflow_error("SubmitBlock request ID exhausted");}
  request.set_id(next_request_id_++);
  auto *payload=request.mutable_submitblockrequest();
  payload->mutable_block()->CopyFrom(block);
  payload->set_allownondaablocks(false);
  output.timing.prepare_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
  if(cancelled && cancelled()) {discard_stream();return done();}
  if(!active->wire->Write(request)) {
    output.timing.write_ms=elapsed(phase);
    discard_stream();
    if(submission_ledger_&&!submission_ledger_->observe(ledger_id,SubmissionLedger::State::Unknown))throw std::runtime_error("durable unknown block send failed");
    transition(output,timed_out.load()?SubmissionState::UnknownAfterTimeout:SubmissionState::UnknownOutcome);
    return done();
  }
  output.timing.write_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
  transition(output,SubmissionState::SubmitSent);
  if(submission_ledger_) {
    const auto started=std::chrono::steady_clock::now();
    if(!submission_ledger_->observe(ledger_id,SubmissionLedger::State::Sent))throw std::runtime_error("durable sent block receipt failed");
    output.timing.journal_sent_ms=elapsed(started);
  }
  phase=std::chrono::steady_clock::now();
  protowire::KaspadResponse response;
  if(!active->wire->Read(&response)||response.id()!=request.id()||!response.has_submitblockresponse()) {
    output.timing.response_ms=elapsed(phase);phase=std::chrono::steady_clock::now();
    discard_stream();output.timing.finish_ms=elapsed(phase);
    if(submission_ledger_&&!submission_ledger_->observe(ledger_id,SubmissionLedger::State::Unknown))throw std::runtime_error("durable unknown block response failed");
    transition(output,timed_out.load()?SubmissionState::UnknownAfterTimeout:SubmissionState::UnknownOutcome);
    return done();
  }
  output.timing.response_ms=elapsed(phase);
  cancellation.request_stop();cancellation.join();
  // Receipt of the correlated protocol response is the acknowledgement.
  // Keep the bidirectional stream open; do not half-close or Finish here.
  output.timing.finish_ms=0;
  const auto &result = response.submitblockresponse();
  output.reason = result.rejectreason();
  if (result.has_error()) output.node_error = result.error().message().empty()?"node returned an empty RPC error":result.error().message();
  if(submission_ledger_) {
    const auto started=std::chrono::steady_clock::now();
    const auto bytes=response.SerializeAsString();
    const auto receipt=consensus::blake3_hash(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
    const auto state=output.reason==protowire::SubmitBlockResponseMessage::NONE && !result.has_error()?
        SubmissionLedger::State::Acknowledged:SubmissionLedger::State::Rejected;
    if(!submission_ledger_->observe(ledger_id,state,receipt))throw std::runtime_error("durable block receipt transition failed");
    output.timing.journal_receipt_ms=elapsed(started);
  }
  transition(output, output.reason == protowire::SubmitBlockResponseMessage::NONE &&
                         !result.has_error()
                     ? SubmissionState::RpcAcknowledged
                     : SubmissionState::RpcRejected);
  return done();
}

SubmitBlockResult SubmitBlockClient::observe_chain(
    SubmitBlockResult result, std::chrono::seconds timeout,
    std::chrono::milliseconds poll_interval, std::uint64_t confirmation_depth,
    const std::function<bool()> &cancelled) {
  if ((result.state != SubmissionState::RpcAcknowledged &&
       result.state != SubmissionState::ChainObserved &&
       result.state != SubmissionState::ChainConfirmed &&
       result.state != SubmissionState::UnknownAfterTimeout &&
       result.state != SubmissionState::UnknownOutcome) || result.block_hash.empty())
    throw std::invalid_argument("chain observation requires a submitted candidate hash");
  if (timeout <= std::chrono::seconds::zero() || poll_interval <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("chain observation timeout and interval must be positive");

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool previously_selected = false;
  auto observation_channel = make_channel(endpoint_);
  while (std::chrono::steady_clock::now() < deadline && !(cancelled && cancelled())) {
    try {
      const auto observation = observe_once(endpoint_, result.block_hash,
          std::chrono::system_clock::now() + std::min(std::chrono::steady_clock::duration(std::chrono::seconds(3)),
              deadline-std::chrono::steady_clock::now()),
          observation_channel);
      if (observation.found) {
        result.block_blue_score = observation.blue_score;
        result.selected_tip_blue_score = observation.selected_tip_blue_score;
        result.currently_selected_chain = observation.selected_chain;
        if (previously_selected && !observation.selected_chain) result.reorg_observed = true;
        previously_selected = observation.selected_chain;
        if(submission_ledger_) {
          consensus::Hash id{};
          if(result.block_hash.size()!=64)throw std::runtime_error("invalid durable block hash");
          for(std::size_t i=0;i<32;++i){auto n=[](char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};const int a=n(result.block_hash[2*i]),b=n(result.block_hash[2*i+1]);if(a<0||b<0)throw std::runtime_error("invalid durable block hash");id[i]=static_cast<std::uint8_t>(16*a+b);}
          if(submission_ledger_->lookup(id)) {
            const std::string data=result.block_hash+":"+std::to_string(observation.blue_score)+":"+std::to_string(observation.selected_tip_blue_score)+":"+(observation.selected_chain?"selected":"dag");
            const auto receipt=consensus::blake3_hash(std::span(reinterpret_cast<const std::uint8_t*>(data.data()),data.size()));
            if(!submission_ledger_->observe(id,SubmissionLedger::State::ChainObserved,receipt))throw std::runtime_error("durable chain observation transition failed");
          }
        }
        transition(result, SubmissionState::ChainObserved);
        const auto depth = observation.selected_tip_blue_score >= observation.blue_score
            ? observation.selected_tip_blue_score - observation.blue_score : 0;
        if (observation.selected_chain && depth >= confirmation_depth) {
          transition(result, SubmissionState::ChainConfirmed);
          return result;
        }
      }
    } catch (const std::exception &) {
      // Retain the last verified state and retry until the bounded observer
      // deadline. No resubmission is attempted here.
    }
    const auto wake = std::min(deadline, std::chrono::steady_clock::now() + poll_interval);
    while (std::chrono::steady_clock::now() < wake && !(cancelled && cancelled()))
      std::this_thread::sleep_for(std::min(std::chrono::milliseconds(25),
          std::chrono::duration_cast<std::chrono::milliseconds>(wake-std::chrono::steady_clock::now())));
  }
  // Absence of a chain observation supplies no transport acknowledgement.
  // Preserve UNKNOWN (or the last actually observed state) at the deadline.
  return result;
}

} // namespace keryx::nexus::network
