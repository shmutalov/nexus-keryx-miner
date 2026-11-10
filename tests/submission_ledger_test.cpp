#include "keryx/nexus/submission_ledger.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using Ledger = keryx::nexus::network::SubmissionLedger;
using Hash = Ledger::Hash;
using State = Ledger::State;
namespace fs = std::filesystem;
#ifndef _WIN32
std::atomic_bool fail_sync{};
extern "C" int fsync(int fd) {
  if (fail_sync.exchange(false)) {
    errno = EIO;
    return -1;
  }
  return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
Hash h(unsigned n) {
  Hash v{};
  v[0] = static_cast<std::uint8_t>(n);
  return v;
}
std::string bytes(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
template <class F> void refused(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, "unsafe ledger accepted");
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "usage: submission-ledger-test persistent-root");
    auto root = fs::absolute(argv[1]);
    check(!root.string().starts_with("/tmp/"), "persistent test root required");
    root /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(root);
    auto path = root / "shares.journal";
    {
      Ledger a(path);
      check(a.reserve(h(1), h(101)), "initial intent failed");
      check(a.lookup(h(1)) == State::Unknown, "intent invented an ACK");
      check(!a.observe(h(1), State::Acknowledged), "receipt-free ACK accepted");
      check(a.observe(h(1), State::Sent), "sent state lost");
    }
    {
      Ledger a(path);
      check(a.lookup(h(1)) == State::Unknown, "crash after send invented ACK");
      check(!a.reserve(h(1), h(102)),
            "restart resent ambiguous nonce with changed payload");
      check(a.observe(h(1), State::Acknowledged, h(201)),
            "correlated ACK failed");
      check(!a.observe(h(1), State::Rejected, h(202)),
            "terminal receipt overwritten");
      check(a.reserve(h(2), h(102)), "second reserve failed");
      check(a.observe(h(2), State::Unknown), "partial send unknown lost");
    }
    {
      Ledger a(path);
      check(a.lookup(h(1)) == State::Acknowledged &&
                a.lookup(h(2)) == State::Unknown,
            "restart lost known/unknown separation");
      check(!a.reserve(h(1), h(101)) && !a.reserve(h(2), h(102)),
            "known or ambiguous delivery resent");
      check(a.observe(h(2), State::Rejected, h(202)),
            "explicit rejection not retained");
    }
    std::atomic_uint wins{};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
      threads.emplace_back([&] {
        Ledger a(path);
        if (a.reserve(h(3), h(103)))
          ++wins;
      });
    for (auto &t : threads)
      t.join();
    check(wins == 1, "concurrent sessions overlapped durable intent");
#ifndef _WIN32
    std::vector<pid_t> children;
    for (int i = 0; i < 4; ++i) {
      auto pid = fork();
      check(pid >= 0, "fork failed");
      if (pid == 0) {
        try {
          Ledger a(path);
          _exit(a.reserve(h(4), h(104)) ? 10 : 20);
        } catch (...) {
          _exit(30);
        }
      }
      children.push_back(pid);
    }
    unsigned process_wins = 0;
    for (auto pid : children) {
      int s{};
      check(waitpid(pid, &s, 0) == pid && WIFEXITED(s), "ledger child failed");
      check(WEXITSTATUS(s) == 10 || WEXITSTATUS(s) == 20,
            "ledger child corrupt result");
      process_wins += WEXITSTATUS(s) == 10;
    }
    check(process_wins == 1, "cross-process reservation overlapped");
    fs::create_symlink(path, root / "link.journal");
    refused([&] { Ledger a(root / "link.journal"); });
    fs::create_hard_link(path, root / "hard.journal");
    refused([&] { Ledger a(root / "hard.journal"); });
    fs::remove(root / "hard.journal");
    auto fault = root / "fsync-fault.journal";
    {
      Ledger a(fault);
      fail_sync = true;
      refused([&] { a.reserve(h(5), h(105)); });
      refused([&] { a.reserve(h(6), h(106)); });
    }
    {
      Ledger a(fault);
      check(a.lookup(h(5)) == State::Unknown && !a.reserve(h(5), h(105)),
            "fsync error later authorized resend or ACK");
    }
    auto partial = root / "write-fault.journal";
    {
      Ledger initialize(partial);
    }
    auto child = fork();
    check(child >= 0, "fault fork failed");
    if (child == 0) {
      signal(SIGXFSZ, SIG_IGN);
      try {
        Ledger a(partial);
        rlimit limit{100, 100};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
          _exit(40);
        try {
          a.reserve(h(7), h(107));
          _exit(41);
        } catch (...) {
          _exit(fs::file_size(partial) == 100 ? 10 : 42);
        }
      } catch (...) {
        _exit(43);
      }
    }
    int status{};
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 10,
          "partial write did not fail before send authorization");
    auto evidence = bytes(partial);
    refused([&] { Ledger a(partial); });
    check(bytes(partial) == evidence,
          "partial write forensic evidence changed");
#endif
    std::vector<double> timing;
    {
      Ledger ledger(path);
      for (unsigned i = 30; i < 130; ++i) {
        const auto started = std::chrono::steady_clock::now();
        check(ledger.reserve(h(i), h(i + 100)), "timed reserve failed");
        check(ledger.observe(h(i), State::Sent) &&
                  ledger.observe(h(i), State::Acknowledged, h(201)),
              "timed receipt failed");
        timing.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started)
                             .count());
      }
    }
    std::sort(timing.begin(), timing.end());
    std::cout << "SUBMISSION_LEDGER_TIMING samples=" << timing.size()
              << " three_fsync_total_p50_ms=" << timing[50]
              << " three_fsync_total_p99_ms=" << timing[99]
              << " scope=CPU_SYNTHETIC_DISK_ONLY\n";
    auto original = bytes(path);
    check(!original.empty(), "no durable records tested");
    auto corrupt = root / "corrupt.journal";
    fs::copy_file(path, corrupt);
    {
      std::fstream f(corrupt, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(20);
      f.put(static_cast<char>(original[20] ^ 1));
    }
    auto corrupt_bytes = bytes(corrupt);
    refused([&] { Ledger a(corrupt); });
    check(bytes(corrupt) == corrupt_bytes, "checksum failure rewrote original");
    auto truncated = root / "truncated.journal";
    fs::copy_file(path, truncated);
    {
      std::ofstream f(truncated, std::ios::binary | std::ios::app);
      f.put('x');
    }
    auto truncated_bytes = bytes(truncated);
    refused([&] { Ledger a(truncated); });
    check(bytes(truncated) == truncated_bytes,
          "truncated journal repaired without evidence");
    auto replaced = root / "replaced.journal";
    {
      Ledger a(replaced);
      fs::rename(replaced, root / "retained.journal");
      {
        Ledger b(replaced);
      }
      refused([&] { a.reserve(h(8), h(108)); });
    }
    refused([&] { Ledger a("relative.journal"); });
    check(bytes(path) == original, "unrelated refusal mutated valid ledger");
    std::cout
        << "SUBMISSION_LEDGER=PASS durable_intent=true restart_no_resend=true "
           "unknown_not_ack=true receipt_binding=true checksum_chain=true";
#ifndef _WIN32
    std::cout << " process_race=true partial_write_retained=true "
                 "fsync_failure_no_send=true\n";
#else
    std::cout << " process_race=NOT_RUN partial_write=NOT_RUN "
                 "fsync_failure=NOT_RUN\n";
#endif
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
