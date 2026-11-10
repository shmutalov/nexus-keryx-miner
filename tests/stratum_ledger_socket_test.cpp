#include "keryx/nexus/stratum_v4_client.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace keryx::nexus::stratum;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
std::string header() {
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
  h.add_parents()->add_parenthashes(std::string(64, 'e'));
  std::string hex;
  for (unsigned char c : h.SerializeAsString()) {
    hex += "0123456789abcdef"[c >> 4];
    hex += "0123456789abcdef"[c & 15];
  }
  return hex;
}
class Pool {
  int fd_;
  std::atomic_int peer_{-1};
  std::atomic_bool stop_{};
  std::thread thread_;
  static bool send(int fd, std::string s) {
    s += '\n';
    std::size_t at = 0;
    while (at < s.size()) {
      auto n = ::send(fd, s.data() + at, s.size() - at, MSG_NOSIGNAL);
      if (n <= 0)
        return false;
      at += static_cast<std::size_t>(n);
    }
    return true;
  }
  void run() {
    unsigned connection = 0;
    while (!stop_) {
      int peer = ::accept(fd_, nullptr, nullptr);
      if (peer < 0)
        break;
      peer_ = peer;
      ++connection;
      std::string input;
      bool connected = true;
      while (connected && !stop_) {
        char buffer[4096];
        auto n = ::recv(peer, buffer, sizeof(buffer), 0);
        if (n <= 0)
          break;
        input.append(buffer, static_cast<std::size_t>(n));
        std::size_t end;
        while ((end = input.find('\n')) != std::string::npos) {
          auto line = input.substr(0, end);
          input.erase(0, end + 1);
          google::protobuf::Struct d;
          if (!google::protobuf::util::JsonStringToMessage(line, &d).ok()) {
            connected = false;
            break;
          }
          const auto &fields = d.fields();
          auto at = fields.find("method");
          if (at == fields.end())
            continue;
          auto method = at->second.string_value();
          auto id = fields.find("id");
          if (id == fields.end())
            continue;
          auto rpc =
              std::to_string(static_cast<unsigned>(id->second.number_value()));
          if (method == "mining.submit") {
            ++submissions;
            ::shutdown(peer, SHUT_RDWR);
            connected = false;
            break;
          } // Deliberately lose ACK after reading the complete frame.
          if (method == "mining.subscribe")
            connected =
                send(peer, "{\"id\":" + rpc +
                               ",\"result\":[[],\"00\",7],\"error\":null}");
          else if (method == "mining.authorize") {
            connected = send(peer, "{\"id\":" + rpc +
                                       ",\"result\":true,\"error\":null}");
            connected =
                connected &&
                send(peer,
                     "{\"method\":\"mining.set_difficulty\",\"params\":[1]}");
            connected =
                connected &&
                send(peer,
                     "{\"method\":\"mining.notify\",\"params\":[\"job-" +
                         std::to_string(connection) + "\",\"" + header() +
                         "\",\"1700000123\",\"148950000\",16842752,\"\"]}");
          } else
            connected = send(peer, "{\"id\":" + rpc +
                                       ",\"result\":true,\"error\":null}");
        }
      }
      peer_ = -1;
      ::close(peer);
    }
  }

public:
  std::atomic_uint submissions{};
  unsigned short port{};
  Pool() {
    fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd_ >= 0, "pool socket failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
              ::listen(fd_, 8) == 0,
          "pool bind failed");
    socklen_t n = sizeof(addr);
    check(::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &n) == 0,
          "pool port failed");
    port = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~Pool() {
    stop_ = true;
    ::shutdown(fd_, SHUT_RDWR);
    int p = peer_;
    if (p >= 0)
      ::shutdown(p, SHUT_RDWR);
    if (thread_.joinable())
      thread_.join();
    ::close(fd_);
  }
};
VerifiedPoolShare ready(StratumV4Client &client) {
  client.start();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::optional<StratumPoolJob> j;
  while (!(j = client.current_job()) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  check(j.has_value(), "pool did not deliver job");
  VerifiedPoolShare s;
  s.job_id = j->job_id;
  s.job_generation = j->generation;
  s.protocol_version = j->protocol_version;
  s.nonce = 7;
  s.timestamp = j->timestamp;
  s.daa_score = j->daa_score;
  s.pre_pow = j->pre_pow;
  s.effective_target = j->effective_target;
  s.network_target = j->network_target;
  s.pom_proof_borsh.resize(73);
  return s;
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "usage: stratum-ledger-socket-test persistent-root");
    auto root = std::filesystem::absolute(argv[1]);
    check(!root.string().starts_with("/tmp/"), "persistent root required");
    root /= std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    Pool pool;
    StratumClientConfig cfg;
    cfg.pool_url = "stratum+tcp://127.0.0.1:" + std::to_string(pool.port);
    cfg.worker = "synthetic";
    cfg.submission_journal = root / "shares.journal";
    cfg.reconnect_initial_ms = 20;
    cfg.reconnect_max_ms = 100;
    {
      StratumV4Client client(cfg);
      auto share = ready(client);
      check(client.submit_share(share).has_value(),
            "first network send failed");
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (pool.submissions == 0 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      check(pool.submissions == 1, "pool did not receive complete submit");
      client.stop();
      check(client.telemetry_snapshot().accepted_shares == 0,
            "lost ACK invented acceptance");
    }
    {
      StratumV4Client client(cfg);
      auto share = ready(client);
      check(!client.submit_share(share),
            "restart resent same proof after lost ACK");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      client.stop();
      check(client.telemetry_snapshot().accepted_shares == 0,
            "restart invented ACK");
    }
    check(pool.submissions == 1, "wire duplicate crossed reconnect boundary");
    std::cout
        << "STRATUM_LEDGER_SOCKET=PASS complete_submit_received=1 "
           "ack_lost=true reconnect_new_job_id=true retransmissions=0 "
           "fabricated_ack=0 network=LOOPBACK proof=synthetic_transport_only\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
