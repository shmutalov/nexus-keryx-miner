#include "keryx/nexus/submission_ledger.hpp"
#include "keryx/nexus/platform/io.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/file.h>
#endif
namespace keryx::nexus::network {
namespace {
namespace io = platform::io;
using Hash = SubmissionLedger::Hash;
using State = SubmissionLedger::State;
constexpr std::size_t record_bytes = 177, max_records = 1000000,
                      max_ids = 250000;
using Record = std::array<std::uint8_t, record_bytes>;
void require(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
Hash digest(std::span<const std::uint8_t> b) {
  Hash h{};
  SHA256(b.data(), b.size(), h.data());
  return h;
}
void put(Record &r, std::size_t at, std::uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    r[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t get(const Record &r, std::size_t at, unsigned n) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i)
    v |= std::uint64_t(r[at + i]) << (8 * i);
  return v;
}
Hash hash_at(const Record &r, std::size_t at) {
  Hash h{};
  std::copy_n(r.data() + at, 32, h.data());
  return h;
}
bool allowed(State from, State to) {
  if (to == State::ChainObserved)
    return from != State::ChainObserved;
  return (from == State::Intent &&
          (to == State::Sent || to == State::Unknown ||
           to == State::Acknowledged || to == State::Rejected)) ||
         ((from == State::Sent || from == State::Unknown) &&
          (to == State::Unknown || to == State::Acknowledged ||
           to == State::Rejected));
}
void seek(int fd, std::uint64_t at) {
#ifdef _WIN32
  require(_lseeki64(fd, static_cast<__int64>(at), SEEK_SET) >= 0,
          "submission ledger seek failed");
#else
  require(::lseek(fd, static_cast<off_t>(at), SEEK_SET) >= 0,
          "submission ledger seek failed");
#endif
}
class FileLock {
  int fd_;

public:
  explicit FileLock(int fd) : fd_(fd) {
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
#ifdef _WIN32
      OVERLAPPED o{};
      if (LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(fd_)),
                     LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                     MAXDWORD, MAXDWORD, &o))
        return;
      require(GetLastError() == ERROR_LOCK_VIOLATION,
              "submission ledger lock failed");
#else
      if (::flock(fd_, LOCK_EX | LOCK_NB) == 0)
        return;
      require(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR,
              "submission ledger lock failed");
#endif
      require(std::chrono::steady_clock::now() < until,
              "submission ledger lock timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  ~FileLock() {
#ifdef _WIN32
    OVERLAPPED o{};
    (void)UnlockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(fd_)), 0,
                       MAXDWORD, MAXDWORD, &o);
#else
    (void)::flock(fd_, LOCK_UN);
#endif
  }
};
} // namespace
struct SubmissionLedger::Impl {
  struct Entry {
    State state;
    Hash payload;
  };
  std::filesystem::path path;
  int fd{-1};
  std::mutex mutex;
  std::uint64_t offset{}, sequence{};
  Hash tail{};
  std::map<Hash, Entry> entries;
  bool failed{};
  explicit Impl(std::filesystem::path p) : path(std::move(p)) {
    require(path.is_absolute(),
            "submission ledger requires an absolute private path");
    if (std::filesystem::create_directories(path.parent_path()))
      std::filesystem::permissions(path.parent_path(),
                                   std::filesystem::perms::owner_all);
    bool created = true;
    fd = io::open(path.c_str(),
                  O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 && errno == EEXIST) {
      created = false;
      fd = io::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    require(fd >= 0, "cannot open private submission ledger");
    try {
      FileLock lock(fd);
      reload();
      if (created) {
        require(io::fsync(fd) == 0, "submission ledger initial fsync failed");
        int parent = io::open(path.parent_path().c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        require(parent >= 0, "submission ledger directory open failed");
        const int synced = io::fsync(parent), closed = io::close(parent);
        require(synced == 0 && closed == 0,
                "submission ledger directory fsync failed");
      }
    } catch (...) {
      io::close(fd);
      fd = -1;
      throw;
    }
  }
  ~Impl() {
    if (fd >= 0)
      io::close(fd);
  }
  void apply(const Record &r) {
    require(std::memcmp(r.data(), "NXSL", 4) == 0 && get(r, 4, 4) == 1 &&
                get(r, 8, 8) == sequence + 1,
            "submission ledger record/sequence invalid");
    require(hash_at(r, 113) == tail &&
                digest(std::span(r.data(), 145)) == hash_at(r, 145),
            "submission ledger checksum chain invalid");
    require(r[16] <= static_cast<unsigned char>(State::ChainObserved),
            "submission ledger state invalid");
    const auto state = static_cast<State>(r[16]);
    const auto id = hash_at(r, 17), payload = hash_at(r, 49),
               receipt = hash_at(r, 81);
    require((state != State::Acknowledged && state != State::Rejected &&
             state != State::ChainObserved) ||
                receipt != Hash{},
            "submission receipt missing");
    auto old = entries.find(id);
    if (old == entries.end()) {
      require(state == State::Intent && entries.size() < max_ids,
              "submission ledger initial state/capacity invalid");
      entries.emplace(id, Entry{state, payload});
    } else {
      require(old->second.payload == payload &&
                  allowed(old->second.state, state),
              "submission ledger transition invalid");
      old->second.state = state;
    }
    ++sequence;
    offset += record_bytes;
    tail = hash_at(r, 145);
  }
  void reload() {
    require(!failed, "submission ledger failed closed");
    io::Stat st{}, name{};
    require(io::fstat(fd, &st) == 0 && io::lstat(path.c_str(), &name) == 0 &&
                S_ISREG(st.st_mode) && !S_ISLNK(name.st_mode) &&
                st.st_dev == name.st_dev && st.st_ino == name.st_ino &&
                st.st_uid == io::getuid() && (st.st_mode & 0077) == 0,
            "submission ledger identity/permissions invalid");
#ifndef _WIN32
    require(st.st_nlink == 1, "submission ledger hardlink refused");
#endif
    require(st.st_size >= 0 &&
                static_cast<std::uint64_t>(st.st_size) >= offset &&
                static_cast<std::uint64_t>(st.st_size) <=
                    max_records * record_bytes &&
                static_cast<std::uint64_t>(st.st_size) % record_bytes == 0,
            "submission ledger truncated or oversized; original retained");
    seek(fd, offset);
    while (offset < static_cast<std::uint64_t>(st.st_size)) {
      Record r{};
      std::size_t at = 0;
      while (at < r.size()) {
        const auto n = io::read(fd, r.data() + at, r.size() - at);
        if (n < 0 && errno == EINTR)
          continue;
        require(n > 0, "submission ledger read failed");
        at += static_cast<std::size_t>(n);
      }
      apply(r);
    }
  }
  void append(const Hash &id, const Hash &payload, State state,
              const Hash &receipt) {
    require(
        sequence < max_records,
        "submission ledger capacity exhausted; original retained for recovery");
    Record r{};
    std::memcpy(r.data(), "NXSL", 4);
    put(r, 4, 1, 4);
    put(r, 8, sequence + 1, 8);
    r[16] = static_cast<std::uint8_t>(state);
    std::copy(id.begin(), id.end(), r.begin() + 17);
    std::copy(payload.begin(), payload.end(), r.begin() + 49);
    std::copy(receipt.begin(), receipt.end(), r.begin() + 81);
    std::copy(tail.begin(), tail.end(), r.begin() + 113);
    const auto check = digest(std::span(r.data(), 145));
    std::copy(check.begin(), check.end(), r.begin() + 145);
    try {
      seek(fd, offset);
      std::size_t at = 0;
      while (at < r.size()) {
        const auto n = io::write(fd, r.data() + at, r.size() - at);
        if (n < 0 && errno == EINTR)
          continue;
        require(n > 0, "submission ledger append failed; no send authorized");
        at += static_cast<std::size_t>(n);
      }
      require(io::fsync(fd) == 0,
              "submission ledger fsync failed; no send authorized");
      apply(r);
    } catch (...) {
      failed = true;
      throw;
    }
  }
};
SubmissionLedger::SubmissionLedger(std::filesystem::path p)
    : impl_(std::make_unique<Impl>(std::move(p))) {}
SubmissionLedger::~SubmissionLedger() = default;
bool SubmissionLedger::reserve(const Hash &id, const Hash &payload) {
  auto &p = *impl_;
  std::lock_guard local(p.mutex);
  FileLock file(p.fd);
  p.reload();
  if (p.entries.contains(id))
    return false;
  require(p.entries.size() < max_ids,
          "submission ledger identity capacity exhausted");
  p.append(id, payload, State::Intent, {});
  return true;
}
bool SubmissionLedger::observe(const Hash &id, State state,
                               const Hash &receipt) {
  auto &p = *impl_;
  std::lock_guard local(p.mutex);
  FileLock file(p.fd);
  p.reload();
  auto at = p.entries.find(id);
  if (at == p.entries.end() || state == State::Intent)
    return false;
  if (at->second.state == state)
    return true;
  if (!allowed(at->second.state, state))
    return false;
  if ((state == State::Acknowledged || state == State::Rejected ||
       state == State::ChainObserved) &&
      receipt == Hash{})
    return false;
  p.append(id, at->second.payload, state, receipt);
  return true;
}
std::optional<SubmissionLedger::State>
SubmissionLedger::lookup(const Hash &id) {
  auto &p = *impl_;
  std::lock_guard local(p.mutex);
  FileLock file(p.fd);
  p.reload();
  auto at = p.entries.find(id);
  if (at == p.entries.end())
    return std::nullopt;
  return at->second.state == State::Intent || at->second.state == State::Sent
             ? State::Unknown
             : at->second.state;
}
} // namespace keryx::nexus::network
