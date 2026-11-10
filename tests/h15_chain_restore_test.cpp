#include "keryx/nexus/h15_bridge.h"
#include "keryx/nexus/h15_chain_restore.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using namespace keryx::nexus::ai;
using Hash = H15ChainRestorePage::Hash;
void check(bool ok, const char *reason) {
  if (!ok)
    throw std::runtime_error(reason);
}
Hash h(unsigned n) {
  Hash a{};
  a[0] = static_cast<std::uint8_t>(n);
  return a;
}
std::string hex(const Hash &a) {
  std::string s;
  for (auto b : a) {
    s += "0123456789abcdef"[b >> 4];
    s += "0123456789abcdef"[b & 15];
  }
  return s;
}
protowire::RpcBlock block(const Hash &id) {
  protowire::RpcBlock b;
  b.mutable_header()->set_daascore(100 + id[0]);
  b.mutable_verbosedata()->set_hash(hex(id));
  b.mutable_verbosedata()->set_ischainblock(true);
  return b;
}
protowire::GetVirtualChainFromBlockResponseMessage page(int n) {
  protowire::GetVirtualChainFromBlockResponseMessage p;
  p.add_removedchainblockhashes(hex(h(240)));
  for (int i = 1; i <= n; ++i) {
    p.add_addedchainblockhashes(hex(h(static_cast<unsigned>(i))));
    auto *a = p.add_acceptedtransactionids();
    a->set_acceptingblockhash(hex(h(static_cast<unsigned>(i))));
    a->add_acceptedtransactionids(hex(h(static_cast<unsigned>(i + 100))));
  }
  return p;
}
struct Frame {
  Hash start{};
  std::uint64_t daa{};
  unsigned removed{};
  std::vector<unsigned> blocks, ids;
  bool last{};
};
Frame decode(std::span<const std::uint8_t> b) {
  Frame f;
  std::size_t at = 0;
  auto number = [&](unsigned n) {
    check(at + n <= b.size(), "truncated frame");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v |= std::uint64_t(b[at++]) << (8 * i);
    return v;
  };
  auto hash = [&]() {
    Hash a{};
    for (auto &v : a)
      v = static_cast<std::uint8_t>(number(1));
    return a;
  };
  f.start = hash();
  f.daa = number(8);
  check(number(4) == 0, "unexpected acceptance blocks");
  f.removed = static_cast<unsigned>(number(4));
  for (unsigned i = 0; i < f.removed; ++i)
    check(hash() == h(240), "wrong removed block");
  auto n = number(4);
  for (std::uint64_t i = 0; i < n; ++i) {
    auto id = hash();
    f.blocks.push_back(id[0]);
    check(number(8) == 100 + id[0], "DAA mismatch");
    auto accepted = number(4);
    check(accepted == 1, "lost accepted IDs");
    f.ids.push_back(hash()[0]);
  }
  f.last = number(1) != 0;
  check(at == b.size(), "trailing frame bytes");
  return f;
}
int main(int argc, char **argv) {
  try {
    // A node taking 800 ms/GetBlock under a 2 s slice cannot finish eight
    // calls. Preserve completed blocks even when the next call exhausts that
    // budget.
    H15ChainRestorePage work(h(0), 100, page(9));
    std::vector<Frame> frames;
    std::vector<std::vector<std::uint8_t>> native_frames;
    int calls = 0, clock = 0;
    while (!work.consumed()) {
      clock = 0;
      work.advance(
          [&](const Hash &id) {
            ++calls;
            clock += 800;
            if (clock > 2000)
              throw H15PollBudgetExhausted();
            return block(id);
          },
          [&](auto bytes) {
            frames.push_back(decode(bytes));
            native_frames.emplace_back(bytes.begin(), bytes.end());
          },
          [&] { return clock < 2000; });
    }
    check(work.committed_blocks() == 9 && work.anchor() == h(9),
          "restore stalled or lost final anchor");
    std::vector<unsigned> ids;
    unsigned removed = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      auto &f = frames[i];
      removed += f.removed;
      check(f.last == (i + 1 == frames.size()),
            "coverage marked complete early");
      check(f.start == h(static_cast<unsigned>(ids.size())),
            "continuation skipped committed anchor");
      for (std::size_t j = 0; j < f.blocks.size(); ++j)
        check(f.ids[j] == f.blocks[j] + 100,
              "accepted IDs mismatched across slice");
      ids.insert(ids.end(), f.blocks.begin(), f.blocks.end());
    }
    check(ids == std::vector<unsigned>({1, 2, 3, 4, 5, 6, 7, 8, 9}) &&
              removed == 1,
          "overlap, skipped blocks or repeated removals");
    check(calls == 13, "unexpected recovery work");
    // A failing native persistence callback cannot advance C++ cursor/anchor.
    H15ChainRestorePage durable(h(0), 100, page(2));
    std::vector<std::uint8_t> failed;
    try {
      durable.advance(
          block,
          [&](auto b) {
            failed.assign(b.begin(), b.end());
            throw std::runtime_error("disk failure");
          },
          [] { return true; });
      check(false, "persistence failure hidden");
    } catch (const std::runtime_error &e) {
      check(std::string(e.what()) == "disk failure", "wrong persistence error");
    }
    check(durable.committed_blocks() == 0 && durable.anchor() == h(0),
          "cursor moved without durable receipt");
    durable.advance(
        block,
        [&](auto b) {
          check(std::vector<std::uint8_t>(b.begin(), b.end()) == failed,
                "retry changed exact canonical frame");
        },
        [] { return true; });
    // A consensus/node failure must not be recast as a successful partial
    // slice.
    H15ChainRestorePage reorg(h(0), 100, page(2));
    unsigned applied = 0;
    bool changed=false;
    try {
      reorg.advance(
          [&](const Hash &id) {
            auto b = block(id);
            if (id[0] == 2)
              b.mutable_verbosedata()->set_ischainblock(false);
            return b;
          },
          [&](auto) { ++applied; }, [] { return true; });
      check(false, "non-selected header admitted");
    } catch (const H15ChainChanged &) {
      changed=true;
    }
    check(changed,"selected-chain change was not typed for prompt refresh");
    H15ChainRefresh refresh;
    check(refresh.retry()==std::chrono::milliseconds(100),"first refresh delayed");
    for(unsigned i=0;i<20;++i)check(refresh.retry()<=std::chrono::seconds(1),"reorg refresh unbounded");
    refresh.complete();
    check(refresh.retry()==std::chrono::milliseconds(100),"refresh did not reset");
    check(applied == 0 && reorg.committed_blocks() == 0,
          "reorg promoted partial progress");
    H15ChainRestorePage no_budget(h(0), 100, page(2));
    try {
      no_budget.advance(block, [&](auto) { ++applied; }, [] { return false; });
      check(false, "empty budget invented progress");
    } catch (const H15PollBudgetExhausted &) {
    }
    check(applied == 0, "empty budget called native apply");
    H15ChainRestorePage full(h(0), 100, page(100));
    while (!full.consumed())
      full.advance(
          block,
          [&](auto b) {
            check(!decode(b).last,
                  "100-block page incorrectly ended canonical coverage");
          },
          [] { return true; });
    auto empty = page(0);
    H15ChainRestorePage zero(h(0), 100, empty);
    zero.advance(
        block,
        [&](auto b) {
          const auto f = decode(b);
          check(f.last && f.blocks.empty() && f.removed == 1,
                "empty final page failed to apply removals");
        },
        [] { return false; });
    H15ChainRestorePage fork(h(0), 500, page(2));
    fork.advance(
        block,
        [](auto b) { check(decode(b).last, "reorg final page not applied"); },
        [] { return true; });
    auto bad = page(1);
    bad.set_addedchainblockhashes(0, "bad");
    H15ChainRestorePage malformed(h(0), 100, bad);
    try {
      malformed.advance(block, [&](auto) { ++applied; }, [] { return true; });
      check(false, "malformed hash accepted");
    } catch (const std::runtime_error &) {
    }
    check(applied == 0, "bad frame applied");
    check(argc == 2, "usage: h15-chain-restore-test persistent-test-root");
    auto root = std::filesystem::absolute(argv[1]);
    check(!root.string().starts_with("/tmp/"), "test root must be persistent");
    root /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    NexusH15Api api{};
    api.chunks = 32;
    std::fill_n(api.model_root, 32, 0x55);
    Hash secret{};
    secret.fill(1);
    Hash model{};
    model.fill(0x44);
    void *raw = nullptr;
    const auto path = (root / "synthetic-native-service").string();
    check(nexus_h15_service_open(
              reinterpret_cast<const std::uint8_t *>(path.data()), path.size(),
              secret.data(), 0, &api, model.data(), &raw) == 0,
          "native service fixture failed");
    std::unique_ptr<void, decltype(&nexus_h15_service_free)> service(
        raw, nexus_h15_service_free);
    for (std::size_t i = 0; i < native_frames.size(); ++i) {
      auto &f = native_frames[i];
      check(nexus_h15_service_chain(raw, f.data(), f.size()) == 0,
            "real Rust bridge rejected restore slice wire");
      check(nexus_h15_service_restoring(raw) ==
                (i + 1 < native_frames.size() ? 1 : 0),
            "native coverage cleared before final slice");
    }
    // The canonical frame can be complete while the following awaited-ledger
    // RPC runs out of budget. Native restoring=false must not reopen replay.
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point{} + std::chrono::seconds(60);
    Clock::time_point next{};
    unsigned ledger_calls = 0;
    try {
      h15_success_throttled_poll(next, now, [&] {
        ++ledger_calls;
        throw H15PollBudgetExhausted();
      });
    } catch (const H15PollBudgetExhausted &) {
    }
    check(next == Clock::time_point{} && ledger_calls == 1,
          "failed node ledger poll armed a successful cooldown");
    h15_success_throttled_poll(next, now + std::chrono::milliseconds(20),
                               [&] { ++ledger_calls; });
    check(ledger_calls == 2,
          "missing node ledger poll was not retried promptly");
    h15_success_throttled_poll(next, now + std::chrono::milliseconds(40),
                               [&] { ++ledger_calls; });
    check(ledger_calls == 2, "successful ledger poll was not throttled");
    H15PollAdmission admission;
    try {
      auto poll = admission.begin();
      check(nexus_h15_service_restoring(raw) == 0,
            "native fixture is not complete");
      throw H15PollBudgetExhausted();
      poll.complete();
    } catch (const H15PollBudgetExhausted &) {
    }
    check(admission.blocked(),
          "ledger RPC budget yield reopened canonical admission");
    {
      auto poll = admission.begin();
      check(admission.blocked(), "new poll skipped the admission barrier");
      poll.complete();
    }
    check(!admission.blocked(), "complete chain+ledger poll did not resume");
    try {
      auto poll = admission.begin();
      throw std::runtime_error("node error");
      poll.complete();
    } catch (const std::runtime_error &) {
    }
    check(admission.blocked(), "node error reopened canonical admission");
    auto corrupt = native_frames.back();
    corrupt.push_back(0);
    check(nexus_h15_service_chain(raw, corrupt.data(), corrupt.size()) != 0,
          "native bridge accepted trailing wire bytes");
    std::cout << "H15_CHAIN_RESTORE=PASS slow_rpc_progress=true "
                 "durable_cursor=true exact_retry=true reorg_fail_closed=true "
                 "removals_once=true full_page_continuation=true "
                 "ledger_budget_holds_replay=true "
                 "failed_poll_not_throttled=true rust_wire=PASS\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
