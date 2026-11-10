#pragma once
#include "rpc.pb.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace keryx::nexus::ai {
// A polling time slice ending is not a node/consensus failure.
struct H15PollBudgetExhausted : std::runtime_error {
  H15PollBudgetExhausted()
      : std::runtime_error("Audit polling budget exhausted") {}
};

// A selected-chain snapshot can change between page and GetBlock calls.
// Refresh it promptly without admitting replay from the invalidated page.
struct H15ChainChanged : std::runtime_error {
  H15ChainChanged() : std::runtime_error("Audit selected chain changed") {}
};
class H15ChainRefresh {
public:
  std::chrono::milliseconds retry() {
    failures_ = std::min(failures_ + 1, 5U);
    return std::chrono::milliseconds(std::min(1000U, 100U << (failures_ - 1)));
  }
  void complete() { failures_ = 0; }
private:
  unsigned failures_{};
};

// Chain-frame completion alone does not admit replay: the node's awaited
// ledger poll must also finish. Unwinding/yielding keeps admission closed.
class H15PollAdmission {
public:
  class Cycle {
  public:
    explicit Cycle(bool &blocked) : blocked_(blocked) { blocked_ = true; }
    Cycle(const Cycle &) = delete;
    void complete() { blocked_ = false; }

  private:
    bool &blocked_;
  };
  Cycle begin() { return Cycle(blocked_); }
  bool blocked() const { return blocked_; }

private:
  bool blocked_{};
};

// A failed ledger RPC must remain due. Starting its cooldown before success
// would let the next restore poll skip the missing ledger check entirely.
template <class Time, class Work>
void h15_success_throttled_poll(Time &next, Time now, Work &&work) {
  if (now < next)
    return;
  std::forward<Work>(work)();
  next = now + std::chrono::seconds(10);
}

// A page is advanced only after the exact validated frame is durably accepted.
// No GPU, local deadline estimate or inferred chain acceptance is involved
// here.
class H15ChainRestorePage {
public:
  using Hash = std::array<std::uint8_t, 32>;
  H15ChainRestorePage(Hash start, std::uint64_t daa,
                      protowire::GetVirtualChainFromBlockResponseMessage page)
      : start_(start), daa_(daa), page_(std::move(page)) {
    if (page_.has_error() || page_.addedchainblockhashes_size() > 10000 ||
        page_.removedchainblockhashes_size() > 72000 ||
        page_.acceptedtransactionids_size() > 10000)
      throw std::runtime_error("H15 chain page bounds");
  }
  const Hash &anchor() const noexcept { return start_; }
  int committed_blocks() const noexcept { return at_; }
  bool consumed() const noexcept {
    return at_ == page_.addedchainblockhashes_size();
  }

  template <class Fetch, class Apply, class HasBudget>
  int advance(Fetch fetch, Apply apply, HasBudget has_budget) {
    const int limit = std::min(at_ + 8, page_.addedchainblockhashes_size());
    std::vector<std::uint8_t> blocks;
    auto last = start_;
    auto last_daa = daa_;
    int end = at_;
    int cursor = accepted_cursor_;
    for (; end < limit; ++end) {
      if (!has_budget())
        break;
      const auto &text = page_.addedchainblockhashes(end);
      const auto h = decode(text);
      protowire::RpcBlock block;
      try {
        block = fetch(h);
      } catch (const H15PollBudgetExhausted &) {
        break;
      }
      if (!block.has_header() || !block.has_verbosedata() ||
          block.verbosedata().hash() != text ||
          block.header().daascore() == 0 ||
          ((end > at_ || at_ > 0) && block.header().daascore() < last_daa))
        throw std::runtime_error("Audit header is not selected chain");
      if (!block.verbosedata().ischainblock())
        throw H15ChainChanged();
      hash(blocks, h);
      put(blocks, block.header().daascore(), 8);
      const protowire::RpcAcceptedTransactionIds *accepted = nullptr;
      if (cursor < page_.acceptedtransactionids_size() &&
          page_.acceptedtransactionids(cursor).acceptingblockhash() == text)
        accepted = &page_.acceptedtransactionids(cursor++);
      else
        for (int i = 0; i < page_.acceptedtransactionids_size(); ++i)
          if (page_.acceptedtransactionids(i).acceptingblockhash() == text) {
            accepted = &page_.acceptedtransactionids(i);
            cursor = i + 1;
            break;
          }
      if (accepted && accepted->acceptedtransactionids_size() > 50000)
        throw std::runtime_error("H15 accepted transaction bounds");
      put(blocks,
          accepted ? static_cast<std::uint64_t>(
                         accepted->acceptedtransactionids_size())
                   : 0,
          4);
      if (accepted)
        for (const auto &id : accepted->acceptedtransactionids())
          hash(blocks, decode(id));
      last = h;
      last_daa = block.header().daascore();
    }
    if (end == at_ && !consumed())
      throw H15PollBudgetExhausted();
    std::vector<std::uint8_t> frame;
    hash(frame, start_);
    put(frame, daa_, 8);
    put(frame, 0, 4);
    put(frame,
        at_ == 0
            ? static_cast<std::uint64_t>(page_.removedchainblockhashes_size())
            : 0,
        4);
    if (at_ == 0)
      for (const auto &id : page_.removedchainblockhashes())
        hash(frame, decode(id));
    put(frame, static_cast<std::uint64_t>(end - at_), 4);
    frame.insert(frame.end(), blocks.begin(), blocks.end());
    put(frame,
        end == page_.addedchainblockhashes_size() &&
            page_.addedchainblockhashes_size() < 100,
        1);
    apply(std::span<const std::uint8_t>(frame));
    const int completed = end - at_;
    start_ = last;
    daa_ = last_daa;
    at_ = end;
    accepted_cursor_ = cursor;
    return completed;
  }

private:
  static Hash decode(std::string_view s) {
    if (s.size() != 64)
      throw std::runtime_error("Invalid audit hash");
    Hash h{};
    for (std::size_t i = 0; i < 32; ++i) {
      auto n = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                      : -1;
      };
      const int a = n(s[2 * i]), b = n(s[2 * i + 1]);
      if (a < 0 || b < 0)
        throw std::runtime_error("Invalid audit hash");
      h[i] = static_cast<std::uint8_t>(a * 16 + b);
    }
    return h;
  }
  static void put(std::vector<std::uint8_t> &b, std::uint64_t n,
                  unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
      b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
  static void hash(std::vector<std::uint8_t> &b, const Hash &h) {
    b.insert(b.end(), h.begin(), h.end());
  }
  Hash start_;
  std::uint64_t daa_;
  protowire::GetVirtualChainFromBlockResponseMessage page_;
  int at_{}, accepted_cursor_{};
};
} // namespace keryx::nexus::ai
