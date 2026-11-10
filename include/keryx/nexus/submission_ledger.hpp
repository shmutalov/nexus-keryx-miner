#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <filesystem>
#include <memory>
#include <optional>

namespace keryx::nexus::network {
// Durable transport receipts, never selected-chain confirmation or payment.
class SubmissionLedger final {
public:
  using Hash = consensus::Hash;
  enum class State : unsigned char {
    Intent = 0,
    Sent = 1,
    Acknowledged = 2,
    Rejected = 3,
    Unknown = 4,
    ChainObserved = 5
  };
  explicit SubmissionLedger(std::filesystem::path);
  ~SubmissionLedger();
  SubmissionLedger(const SubmissionLedger &) = delete;
  SubmissionLedger &operator=(const SubmissionLedger &) = delete;
  // One durable reservation across processes/reconnects/restarts. False means
  // already reserved, including unknown delivery. Never retransmit implicitly.
  bool reserve(const Hash &id, const Hash &payload_digest);
  bool observe(const Hash &id, State, const Hash &receipt_digest = {});
  // An intent/sent record without a receipt is UNKNOWN, including after crash.
  std::optional<State> lookup(const Hash &id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace keryx::nexus::network
