#include "keryx/nexus/stratum_v4_client.hpp"

#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/protocol_rules.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <basetsd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
using socket_t = SOCKET;
using pollfd_t = WSAPOLLFD;
using ssize_t = SSIZE_T;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
using pollfd_t = pollfd;
#endif

namespace keryx::nexus::stratum {

namespace {

constexpr std::size_t kMaxStratumLineBytes = 256 * 1024;
constexpr std::size_t kMaxWindowSeedCache = 16;
constexpr std::size_t kMaxSeenJobs = 128;

#ifdef _WIN32
void ensure_winsock_initialized() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    WSADATA wsa;
    (void)::WSAStartup(MAKEWORD(2, 2), &wsa);
  });
}
#endif

void close_socket_fd(socket_t &fd) noexcept {
#ifdef _WIN32
  if (fd != INVALID_SOCKET) {
    ::shutdown(fd, SD_BOTH);
    ::closesocket(fd);
    fd = INVALID_SOCKET;
  }
#else
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    fd = -1;
  }
#endif
}

bool set_socket_nonblocking(socket_t fd, bool nonblocking) {
#ifdef _WIN32
  u_long mode = nonblocking ? 1 : 0;
  return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) >= 0;
#endif
}

int socket_poll(pollfd_t *fds, unsigned long nfds, int timeout_ms) {
#ifdef _WIN32
  return ::WSAPoll(fds, nfds, timeout_ms);
#else
  return ::poll(fds, nfds, timeout_ms);
#endif
}

bool is_socket_valid(socket_t fd) noexcept {
#ifdef _WIN32
  return fd != INVALID_SOCKET;
#else
  return fd >= 0;
#endif
}

void need(bool ok, const char *message) {
  if (!ok) {
    throw std::runtime_error(message);
  }
}

int hex_nibble(char c) noexcept {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char raw_ch : input) {
    const auto c = static_cast<unsigned char>(raw_ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        std::ostringstream oss;
        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<unsigned>(c);
        out += oss.str();
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return out;
}

std::string base64_encode(std::span<const std::uint8_t> in) {
  static constexpr char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= in.size()) {
    const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                            (static_cast<std::uint32_t>(in[i + 1]) << 8) |
                            static_cast<std::uint32_t>(in[i + 2]);
    out.push_back(tbl[(v >> 18) & 0x3f]);
    out.push_back(tbl[(v >> 12) & 0x3f]);
    out.push_back(tbl[(v >> 6) & 0x3f]);
    out.push_back(tbl[v & 0x3f]);
    i += 3;
  }
  if (i + 1 == in.size()) {
    const std::uint32_t v = static_cast<std::uint32_t>(in[i]) << 16;
    out.push_back(tbl[(v >> 18) & 0x3f]);
    out.push_back(tbl[(v >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == in.size()) {
    const std::uint32_t v = (static_cast<std::uint32_t>(in[i]) << 16) |
                            (static_cast<std::uint32_t>(in[i + 1]) << 8);
    out.push_back(tbl[(v >> 18) & 0x3f]);
    out.push_back(tbl[(v >> 12) & 0x3f]);
    out.push_back(tbl[(v >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

std::string u64_hex_padded(std::uint64_t val, std::size_t digits) {
  std::ostringstream oss;
  oss << std::hex << std::nouppercase << std::setfill('0');
  if (digits > 0) {
    oss << std::setw(static_cast<int>(digits));
  }
  oss << val;
  std::string s = oss.str();
  if (digits > 0 && s.size() > digits) {
    return s.substr(s.size() - digits);
  }
  return s;
}

std::optional<std::uint64_t>
value_to_u64(const google::protobuf::Value &v) noexcept {
  if (v.kind_case() == google::protobuf::Value::kNumberValue) {
    const double d = v.number_value();
    if (!std::isfinite(d) || d < 0.0 ||
        d > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
      return std::nullopt;
    }
    const auto u = static_cast<std::uint64_t>(d);
    if (static_cast<double>(u) != std::floor(d)) {
      return std::nullopt;
    }
    return u;
  }
  if (v.kind_case() == google::protobuf::Value::kStringValue) {
    const auto &s = v.string_value();
    if (s.empty())
      return std::nullopt;
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(s.c_str(), &end, 10);
    if (errno == 0 && end == s.c_str() + s.size()) {
      return static_cast<std::uint64_t>(parsed);
    }
  }
  return std::nullopt;
}

} // namespace

std::string_view to_string(ConnectionState state) noexcept {
  switch (state) {
  case ConnectionState::Disconnected:
    return "DISCONNECTED";
  case ConnectionState::Connecting:
    return "CONNECTING";
  case ConnectionState::Connected:
    return "CONNECTED";
  case ConnectionState::Subscribed:
    return "SUBSCRIBED";
  case ConnectionState::Authorized:
    return "AUTHORIZED";
  case ConnectionState::Ready:
    return "CONNECTED";
  case ConnectionState::Reconnecting:
    return "RECONNECTING";
  case ConnectionState::Error:
    return "ERROR";
  }
  return "DISCONNECTED";
}

bool is_stratum_endpoint_url(std::string_view url) noexcept {
  return url.starts_with("stratum+tcp://") ||
         url.starts_with("stratum+ssl://") ||
         url.starts_with("stratum+tls://") || url.starts_with("stratum://");
}

StratumEndpoint parse_stratum_endpoint(std::string_view url) {
  std::string raw(url);
  while (!raw.empty() &&
         std::isspace(static_cast<unsigned char>(raw.front()))) {
    raw.erase(raw.begin());
  }
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
    raw.pop_back();
  }
  need(!raw.empty(), "Empty Stratum pool URL");

  StratumEndpoint ep;
  ep.raw_url = raw;
  std::string rest = raw;
  if (const auto pos = rest.find("://"); pos != std::string::npos) {
    ep.scheme = rest.substr(0, pos);
    for (auto &c : ep.scheme)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    rest = rest.substr(pos + 3);
  } else {
    ep.scheme = "stratum+tcp";
  }

  if (ep.scheme == "stratum+ssl" || ep.scheme == "stratum+tls" ||
      ep.scheme == "tls" || ep.scheme == "ssl") {
    ep.tls = true;
  } else if (ep.scheme == "stratum+tcp" || ep.scheme == "stratum" ||
             ep.scheme == "tcp") {
    ep.tls = false;
  } else {
    throw std::runtime_error("Unsupported Stratum URL scheme: " + ep.scheme);
  }

  if (const auto slash = rest.find('/'); slash != std::string::npos) {
    rest = rest.substr(0, slash);
  }
  const auto colon = rest.rfind(':');
  need(colon != std::string::npos && colon > 0 && colon + 1 < rest.size(),
       "Stratum pool URL must include host:port");
  ep.host = rest.substr(0, colon);
  if (ep.host.size() >= 2 && ep.host.front() == '[' && ep.host.back() == ']') {
    ep.host = ep.host.substr(1, ep.host.size() - 2);
  }
  const std::string port_str = rest.substr(colon + 1);
  for (char c : port_str) {
    need(std::isdigit(static_cast<unsigned char>(c)) != 0,
         "Invalid Stratum port");
  }
  const unsigned long p = std::stoul(port_str);
  need(p >= 1 && p <= 65535, "Stratum port out of range");
  ep.port = static_cast<std::uint16_t>(p);
  return ep;
}

std::string hex_encode_bytes(std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out[2 * i] = kHex[(bytes[i] >> 4) & 0x0f];
    out[2 * i + 1] = kHex[bytes[i] & 0x0f];
  }
  return out;
}

std::vector<std::uint8_t> hex_decode_bytes(std::string_view hex) {
  if (hex.starts_with("0x") || hex.starts_with("0X")) {
    hex.remove_prefix(2);
  }
  need(hex.size() % 2 == 0, "Hex string must have even length");
  std::vector<std::uint8_t> out(hex.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = hex_nibble(hex[2 * i]);
    const int lo = hex_nibble(hex[2 * i + 1]);
    need(hi >= 0 && lo >= 0, "Invalid hex character");
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

Hash parse_hash32_hex(std::string_view hex) {
  const auto bytes = hex_decode_bytes(hex);
  need(bytes.size() == 32, "Expected 32-byte (64-hex) hash");
  Hash h{};
  std::memcpy(h.data(), bytes.data(), 32);
  return h;
}

int compare_target_u256(const Hash &lhs_le, const Hash &rhs_le) noexcept {
  for (int i = 31; i >= 0; --i) {
    if (lhs_le[static_cast<std::size_t>(i)] <
        rhs_le[static_cast<std::size_t>(i)])
      return -1;
    if (lhs_le[static_cast<std::size_t>(i)] >
        rhs_le[static_cast<std::size_t>(i)])
      return 1;
  }
  return 0;
}

Hash difficulty_to_target(double difficulty) {
  need(std::isfinite(difficulty) && difficulty > 0.0,
       "Stratum difficulty must be finite and positive");
  // DIFF1 target is 0xffff << 208 (28 bytes = 224 bits, matching
  // keryx-stratum-bridge maxTarget and keryx-miner target_from_difficulty).
  // Compute target = floor((0xffff << 208) / difficulty) using OpenSSL BIGNUM.
  int exp = 0;
  const double mantissa = std::frexp(difficulty, &exp);
  // difficulty = mantissa * 2^exp, where mantissa in [0.5, 1.0)
  // Scale mantissa by 2^53 so it is an exact 53-bit integer:
  // difficulty = scaled_m * 2^(exp - 53)
  const auto scaled_m =
      static_cast<std::uint64_t>( std::ldexp(mantissa, 53) );
  need(scaled_m > 0, "Invalid difficulty mantissa");

  BN_CTX *ctx = BN_CTX_new();
  BIGNUM *num = BN_new();
  BIGNUM *den = BN_new();
  BIGNUM *quo = BN_new();
  if (!ctx || !num || !den || !quo) {
    if (quo) BN_free(quo);
    if (den) BN_free(den);
    if (num) BN_free(num);
    if (ctx) BN_CTX_free(ctx);
    throw std::runtime_error("OpenSSL BN allocation failed");
  }

  BN_set_word(num, 0xffffULL);
  BN_lshift(num, num, 208);
  BN_set_word(den, scaled_m);

  const int shift = 53 - exp;
  if (shift >= 0) {
    BN_lshift(num, num, shift);
  } else {
    BN_lshift(den, den, -shift);
  }

  BN_div(quo, nullptr, num, den, ctx);

  Hash out{};
  if (BN_num_bytes(quo) > 32) {
    out.fill(0xff);
  } else if (BN_is_zero(quo)) {
    out[0] = 1;
  } else {
    BN_bn2lebinpad(quo, out.data(), 32);
  }

  BN_free(quo);
  BN_free(den);
  BN_free(num);
  BN_CTX_free(ctx);
  return out;
}

extern "C" int nexus_opoi_coinbase_tag(std::uint64_t, unsigned char *);

std::string compute_stratum_opoi_tag(std::uint64_t nonce) {
  std::array<unsigned char, 16> tag{};
  if (nexus_opoi_coinbase_tag(nonce, tag.data()) == 1) {
    return std::string(tag.begin(), tag.end());
  }
  const std::string extra = opoi::coinbase_extra_data("", nonce);
  const auto start = extra.find("/ai:v1:");
  if (start != std::string::npos && start + 7 + 16 <= extra.size()) {
    return extra.substr(start + 7, 16);
  }
  throw std::runtime_error("Failed to compute OPoI tag for nonce");
}

struct StratumV4Client::SocketHandle {
#ifdef _WIN32
  socket_t fd{INVALID_SOCKET};
#else
  socket_t fd{-1};
#endif
  SSL_CTX *ssl_ctx{nullptr};
  SSL *ssl{nullptr};
  std::string rx_buffer;

  ~SocketHandle() { close(); }

  void close() noexcept {
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (ssl_ctx) {
      SSL_CTX_free(ssl_ctx);
      ssl_ctx = nullptr;
    }
    close_socket_fd(fd);
    rx_buffer.clear();
  }
};

StratumV4Client::StratumV4Client(StratumClientConfig config)
    : config_(std::move(config)),
      endpoint_(parse_stratum_endpoint(config_.pool_url)),
      socket_(std::make_unique<SocketHandle>()),
      current_share_target_(difficulty_to_target(1.0)) {
#ifdef _WIN32
  ensure_winsock_initialized();
#endif
  endpoints_.push_back(endpoint_);
  for (const auto &backup_url : config_.backup_urls) {
    if (!backup_url.empty() && backup_url != config_.pool_url) {
      endpoints_.push_back(parse_stratum_endpoint(backup_url));
    }
  }
  need(!config_.worker.empty(), "Stratum worker (wallet.worker) is required");
  if (config_.password.empty()) {
    config_.password = "x";
  }
}

StratumV4Client::~StratumV4Client() { stop(); }

void StratumV4Client::force_failover_for_test() {
  std::lock_guard lock(mutex_);
  if (endpoints_.size() > 1) {
    active_endpoint_index_ = (active_endpoint_index_ + 1) % endpoints_.size();
    endpoint_ = endpoints_[active_endpoint_index_];
    active_job_.reset();
    pending_shares_.clear();
    submitted_nonces_by_job_.clear();
    job_generation_.fetch_add(1, std::memory_order_acq_rel);
    std::cout << "{\"event\":\"STRATUM_FAILOVER\",\"action\":\"TEST_FORCE_FAILOVER\","
              << "\"index\":" << active_endpoint_index_ << ",\"endpoint\":\""
              << json_escape(endpoint_.raw_url) << "\"}\n"
              << std::flush;
  }
}

void StratumV4Client::enable_loopback_test_mode(bool enabled) {
  std::lock_guard lock(mutex_);
  loopback_test_mode_ = enabled;
  if (enabled) {
    state_ = ConnectionState::Ready;
  }
}

std::vector<std::string> StratumV4Client::take_outbound_wire_log() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  out.swap(outbound_wire_log_);
  return out;
}

void StratumV4Client::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }
  stop_requested_.store(false, std::memory_order_release);
  io_thread_ = std::thread([this]() { io_loop(); });
}

void StratumV4Client::stop() {
  stop_requested_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    close_socket_locked();
    cv_.notify_all();
  }
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
  running_.store(false, std::memory_order_release);
}

void StratumV4Client::close_socket_locked() {
  if (socket_) {
    socket_->close();
  }
}

void StratumV4Client::record_error_locked(std::string message) {
  last_error_ = std::move(message);
  std::cout << "{\"event\":\"STRATUM_ERROR\",\"endpoint\":\""
            << json_escape(endpoint_.raw_url) << "\",\"reason\":\""
            << json_escape(last_error_) << "\"}\n"
            << std::flush;
}

bool StratumV4Client::send_line_locked(const std::string &line) {
  std::string framed = line;
  if (framed.empty() || framed.back() != '\n') {
    framed.push_back('\n');
  }
  if (loopback_test_mode_) {
    outbound_wire_log_.push_back(line);
    return true;
  }
  if (!socket_ || !is_socket_valid(socket_->fd)) {
    return false;
  }
  const char *ptr = framed.data();
  std::size_t remaining = framed.size();
  while (remaining > 0) {
    ssize_t written = 0;
    if (socket_->ssl) {
      written = SSL_write(socket_->ssl, ptr, static_cast<int>(remaining));
    } else {
#ifdef _WIN32
      written = ::send(socket_->fd, ptr, static_cast<int>(remaining), 0);
#else
      written = ::send(socket_->fd, ptr, remaining, MSG_NOSIGNAL);
#endif
    }
    if (written <= 0) {
      return false;
    }
    ptr += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool StratumV4Client::connect_and_handshake_once() {
  std::unique_lock lock(mutex_);
  if (loopback_test_mode_) {
    state_ = ConnectionState::Connecting;
  } else {
    close_socket_locked();
    state_ = (reconnect_count_ > 0) ? ConnectionState::Reconnecting
                                    : ConnectionState::Connecting;
    const std::string host = endpoint_.host;
    const std::string port = std::to_string(endpoint_.port);
    const bool use_tls = endpoint_.tls;
    const bool verify_peer = config_.tls_verify_peer;
    const int connect_timeout_ms = config_.connect_timeout_ms;
    const int io_timeout_ms = config_.io_timeout_ms;
    lock.unlock();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0 || !res) {
      lock.lock();
      state_ = ConnectionState::Reconnecting;
      record_error_locked("DNS resolution failed for " + host + ":" + port);
      return false;
    }

    socket_t connected_fd =
#ifdef _WIN32
        INVALID_SOCKET;
#else
        -1;
#endif
    for (addrinfo *p = res; p != nullptr; p = p->ai_next) {
#ifdef _WIN32
      socket_t fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (fd == INVALID_SOCKET)
        continue;
#else
      socket_t fd = ::socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC,
                            p->ai_protocol);
      if (fd < 0)
        continue;
#endif
      set_socket_nonblocking(fd, true);
#ifdef _WIN32
      int rc = ::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen));
#else
      int rc = ::connect(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen));
#endif
      if (rc < 0) {
#ifdef _WIN32
        const int err = ::WSAGetLastError();
        const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        const int err = errno;
        const bool in_progress = (err == EINPROGRESS);
#endif
        if (in_progress) {
          pollfd_t pfd{};
          pfd.fd = fd;
          pfd.events = POLLOUT;
          rc = socket_poll(&pfd, 1, connect_timeout_ms);
          if (rc > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
#ifdef _WIN32
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &len) == 0 &&
                so_error == 0) {
#else
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 &&
                so_error == 0) {
#endif
              connected_fd = fd;
              set_socket_nonblocking(fd, false);
              break;
            }
          }
        }
      } else if (rc == 0) {
        connected_fd = fd;
        set_socket_nonblocking(fd, false);
        break;
      }
      close_socket_fd(fd);
    }
    ::freeaddrinfo(res);

    if (!is_socket_valid(connected_fd)) {
      lock.lock();
      state_ = ConnectionState::Reconnecting;
      record_error_locked("TCP connection failed to " + host + ":" + port);
      return false;
    }

    int one = 1;
#ifdef _WIN32
    ::setsockopt(connected_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    ::setsockopt(connected_fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof(one));
    DWORD tv = static_cast<DWORD>(io_timeout_ms);
    ::setsockopt(connected_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    ::setsockopt(connected_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    ::setsockopt(connected_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(connected_fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    timeval tv{};
    tv.tv_sec = io_timeout_ms / 1000;
    tv.tv_usec = (io_timeout_ms % 1000) * 1000;
    ::setsockopt(connected_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(connected_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    SSL_CTX *ssl_ctx = nullptr;
    SSL *ssl = nullptr;
    if (use_tls) {
      ssl_ctx = SSL_CTX_new(TLS_client_method());
      if (!ssl_ctx) {
        close_socket_fd(connected_fd);
        lock.lock();
        state_ = ConnectionState::Error;
        record_error_locked("TLS context initialization failed");
        return false;
      }
      SSL_CTX_set_verify(ssl_ctx,
                         verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                         nullptr);
      if (verify_peer) {
        SSL_CTX_set_default_verify_paths(ssl_ctx);
      }
      ssl = SSL_new(ssl_ctx);
      SSL_set_tlsext_host_name(ssl, host.c_str());
#ifdef _WIN32
      SSL_set_fd(ssl, static_cast<int>(connected_fd));
#else
      SSL_set_fd(ssl, connected_fd);
#endif
      if (SSL_connect(ssl) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        close_socket_fd(connected_fd);
        lock.lock();
        state_ = ConnectionState::Reconnecting;
        record_error_locked("TLS handshake failed with " + host + ":" + port);
        return false;
      }
    }

    lock.lock();
    if (stop_requested_.load(std::memory_order_acquire)) {
      if (ssl)
        SSL_free(ssl);
      if (ssl_ctx)
        SSL_CTX_free(ssl_ctx);
      close_socket_fd(connected_fd);
      return false;
    }
    socket_->fd = connected_fd;
    socket_->ssl_ctx = ssl_ctx;
    socket_->ssl = ssl;
    socket_->rx_buffer.clear();
    state_ = ConnectionState::Connected;
  }

  active_job_.reset();
  pending_shares_.clear();
  submitted_nonces_by_job_.clear();
  job_generation_.fetch_add(1, std::memory_order_acq_rel);

  const auto now = std::chrono::steady_clock::now();
  subscribe_rpc_id_ = next_rpc_id_++;
  subscribe_sent_at_ = now;
  const std::string sub_msg =
      "{\"id\":" + std::to_string(subscribe_rpc_id_) +
      ",\"method\":\"mining.subscribe\",\"params\":[\"" +
      json_escape(config_.user_agent) + "\",\"" +
      json_escape(config_.protocol_tag) + "\"]}";
  if (!send_line_locked(sub_msg)) {
    close_socket_locked();
    state_ = ConnectionState::Reconnecting;
    record_error_locked("Failed sending mining.subscribe");
    return false;
  }

  authorize_rpc_id_ = next_rpc_id_++;
  authorize_sent_at_ = std::chrono::steady_clock::now();
  const std::string auth_msg =
      "{\"id\":" + std::to_string(authorize_rpc_id_) +
      ",\"method\":\"mining.authorize\",\"params\":[\"" +
      json_escape(config_.worker) + "\",\"" + json_escape(config_.password) +
      "\"]}";
  if (!send_line_locked(auth_msg)) {
    close_socket_locked();
    state_ = ConnectionState::Reconnecting;
    record_error_locked("Failed sending mining.authorize");
    return false;
  }

  if (!config_.declared_model_id_hex.empty()) {
    const std::uint64_t cap_id = next_rpc_id_++;
    const std::string cap_msg =
        "{\"id\":" + std::to_string(cap_id) +
        ",\"method\":\"mining.declare_capabilities\",\"params\":[\"" +
        json_escape(config_.declared_model_id_hex) + "\"]}";
    if (!send_line_locked(cap_msg)) {
      close_socket_locked();
      state_ = ConnectionState::Reconnecting;
      record_error_locked("Failed sending mining.declare_capabilities");
      return false;
    }
  }

  std::cout << "{\"event\":\"STRATUM_CONNECTED\",\"endpoint\":\""
            << json_escape(endpoint_.raw_url) << "\",\"worker\":\""
            << json_escape(config_.worker) << "\",\"protocol\":\""
            << json_escape(config_.protocol_tag) << "\"}\n"
            << std::flush;
  return true;
}

bool StratumV4Client::read_line_once(std::string &out_line) {
  {
    std::lock_guard lock(mutex_);
    if (!socket_ || !is_socket_valid(socket_->fd))
      return false;
    const auto nl = socket_->rx_buffer.find('\n');
    if (nl != std::string::npos) {
      out_line = socket_->rx_buffer.substr(0, nl);
      if (!out_line.empty() && out_line.back() == '\r') {
        out_line.pop_back();
      }
      socket_->rx_buffer.erase(0, nl + 1);
      return true;
    }
  }

  std::array<char, 8192> chunk{};
  ssize_t n = 0;
  socket_t fd =
#ifdef _WIN32
      INVALID_SOCKET;
#else
      -1;
#endif
  SSL *ssl = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (!socket_ || !is_socket_valid(socket_->fd))
      return false;
    fd = socket_->fd;
    ssl = socket_->ssl;
  }

  if (ssl) {
    n = SSL_read(ssl, chunk.data(), static_cast<int>(chunk.size()));
    if (n <= 0) {
      const int err = SSL_get_error(ssl, static_cast<int>(n));
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        out_line.clear();
        return true;
      }
      return false;
    }
  } else {
    pollfd_t pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int prc = socket_poll(&pfd, 1, 250);
    if (prc == 0) {
      out_line.clear();
      return true;
    }
    if (prc < 0) {
#ifdef _WIN32
      if (::WSAGetLastError() == WSAEINTR) {
        out_line.clear();
        return true;
      }
#else
      if (errno == EINTR) {
        out_line.clear();
        return true;
      }
#endif
      return false;
    }
#ifdef _WIN32
    n = ::recv(fd, chunk.data(), static_cast<int>(chunk.size()), 0);
    if (n < 0) {
      const int rerr = ::WSAGetLastError();
      if (rerr == WSAEWOULDBLOCK || rerr == WSAEINTR) {
        out_line.clear();
        return true;
      }
    }
#else
    n = ::recv(fd, chunk.data(), chunk.size(), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      out_line.clear();
      return true;
    }
#endif
    if (n <= 0) {
      return false;
    }
  }

  std::lock_guard lock(mutex_);
  if (!socket_ || !is_socket_valid(socket_->fd))
    return false;
  if (socket_->rx_buffer.size() + static_cast<std::size_t>(n) >
      kMaxStratumLineBytes) {
    record_error_locked("Oversized Stratum line received from pool");
    return false;
  }
  socket_->rx_buffer.append(chunk.data(), static_cast<std::size_t>(n));
  const auto nl = socket_->rx_buffer.find('\n');
  if (nl != std::string::npos) {
    out_line = socket_->rx_buffer.substr(0, nl);
    if (!out_line.empty() && out_line.back() == '\r') {
      out_line.pop_back();
    }
    socket_->rx_buffer.erase(0, nl + 1);
  } else {
    out_line.clear();
  }
  return true;
}

void StratumV4Client::io_loop() {
  int backoff_ms = config_.reconnect_initial_ms;
  int consecutive_failures = 0;
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (!connect_and_handshake_once()) {
      {
        std::unique_lock lock(mutex_);
        ++reconnect_count_;
        ++consecutive_failures;
        state_ = ConnectionState::Reconnecting;
        if (endpoints_.size() > 1 && consecutive_failures >= 2) {
          consecutive_failures = 0;
          active_endpoint_index_ = (active_endpoint_index_ + 1) % endpoints_.size();
          endpoint_ = endpoints_[active_endpoint_index_];
          active_job_.reset();
          pending_shares_.clear();
          submitted_nonces_by_job_.clear();
          job_generation_.fetch_add(1, std::memory_order_acq_rel);
          std::cout << "{\"event\":\"STRATUM_FAILOVER\",\"action\":\"SWITCH_ENDPOINT\","
                    << "\"index\":" << active_endpoint_index_ << ",\"endpoint\":\""
                    << json_escape(endpoint_.raw_url) << "\"}\n"
                    << std::flush;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(backoff_ms), [this]() {
          return stop_requested_.load(std::memory_order_acquire);
        });
      }
      backoff_ms = std::min(config_.reconnect_max_ms, backoff_ms * 2);
      continue;
    }
    consecutive_failures = 0;
    backoff_ms = config_.reconnect_initial_ms;

    while (!stop_requested_.load(std::memory_order_acquire)) {
      std::string line;
      if (!read_line_once(line)) {
        std::lock_guard lock(mutex_);
        close_socket_locked();
        active_job_.reset();
        pending_shares_.clear();
        submitted_nonces_by_job_.clear();
        job_generation_.fetch_add(1, std::memory_order_acq_rel);
        ++reconnect_count_;
        state_ = ConnectionState::Reconnecting;
        record_error_locked("Pool connection lost; reconnecting");
        if (endpoints_.size() > 1) {
          active_endpoint_index_ = (active_endpoint_index_ + 1) % endpoints_.size();
          endpoint_ = endpoints_[active_endpoint_index_];
          std::cout << "{\"event\":\"STRATUM_FAILOVER\",\"action\":\"CONNECTION_LOST_SWITCH\","
                    << "\"index\":" << active_endpoint_index_ << ",\"endpoint\":\""
                    << json_escape(endpoint_.raw_url) << "\"}\n"
                    << std::flush;
        }
        break;
      }
      if (!line.empty()) {
        try {
          process_inbound_line(line);
        } catch (const std::exception &ex) {
          std::lock_guard lock(mutex_);
          record_error_locked(std::string("Stratum message error: ") +
                              ex.what());
        }
      }
    }
  }
}

bool StratumV4Client::process_inbound_line(std::string_view line) {
  while (!line.empty() &&
         std::isspace(static_cast<unsigned char>(line.front()))) {
    line.remove_prefix(1);
  }
  while (!line.empty() &&
         std::isspace(static_cast<unsigned char>(line.back()))) {
    line.remove_suffix(1);
  }
  if (line.empty())
    return false;

  google::protobuf::Struct root;
  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = true;
  const auto status = google::protobuf::util::JsonStringToMessage(
      std::string(line), &root, opts);
  need(status.ok(), "Malformed Stratum JSON message");

  const auto &fields = root.fields();
  const auto method_it = fields.find("method");
  if (method_it != fields.end() &&
      method_it->second.kind_case() ==
          google::protobuf::Value::kStringValue &&
      !method_it->second.string_value().empty()) {
    const std::string &method = method_it->second.string_value();

    if (method == "mining.ping" || method == "ping") {
      const auto id_it = fields.find("id");
      if (id_it != fields.end() &&
          id_it->second.kind_case() != google::protobuf::Value::kNullValue) {
        if (const auto id_opt = value_to_u64(id_it->second)) {
          std::lock_guard lock(mutex_);
          send_line_locked("{\"id\":" + std::to_string(*id_opt) +
                           ",\"result\":\"pong\",\"error\":null}");
        }
      }
      return true;
    }

    const auto params_it = fields.find("params");
    if (params_it == fields.end() ||
        params_it->second.kind_case() != google::protobuf::Value::kListValue) {
      // Notification or method without params list (e.g. keepalive, custom pool method)
      return true;
    }
    const auto &params = params_it->second.list_value().values();

    if (method == "mining.set_difficulty") {
      need(params.size() >= 1 &&
               params[0].kind_case() == google::protobuf::Value::kNumberValue,
           "mining.set_difficulty requires numeric difficulty");
      const double diff = params[0].number_value();
      const Hash target = difficulty_to_target(diff);

      std::lock_guard lock(mutex_);
      current_difficulty_ = diff;
      current_share_target_ = target;
      last_error_.clear();
      if (active_job_.has_value()) {
        active_job_->difficulty = diff;
        active_job_->share_target = target;
        if (active_job_->has_bits &&
            compare_target_u256(active_job_->network_target, target) > 0) {
          active_job_->effective_target = active_job_->network_target;
        } else {
          active_job_->effective_target = target;
        }
        // Stratum difficulty retargeting updates the target threshold on the current job
        // without invalidating the active job or resetting the nonce search cursor.
      }
      std::cout << "{\"event\":\"STRATUM_DIFFICULTY\",\"difficulty\":" << diff
                << "}\n"
                << std::flush;
      return true;
    }

    if (method == "mining.set_extranonce") {
      need(params.size() >= 2 &&
               params[0].kind_case() == google::protobuf::Value::kStringValue,
           "mining.set_extranonce requires extranonce hex and size");
      const std::string &en_hex = params[0].string_value();
      const auto en2_bytes_opt = value_to_u64(params[1]);
      need(en2_bytes_opt.has_value() && *en2_bytes_opt <= 8,
           "Invalid extranonce2 size");
      const auto en_bytes = hex_decode_bytes(en_hex);
      need(en_bytes.size() + *en2_bytes_opt <= 8,
           "Extranonce exceeds 64-bit nonce space");
      const std::size_t en2_bits = (*en2_bytes_opt) * 8;
      std::uint64_t fixed = 0;
      for (std::uint8_t b : en_bytes) {
        fixed = (fixed << 8) | static_cast<std::uint64_t>(b);
      }
      if (en2_bits < 64) {
        fixed <<= en2_bits;
      } else {
        fixed = 0;
      }
      const std::uint64_t mask =
          (en2_bits >= 64) ? 0xffffffffffffffffULL : ((1ULL << en2_bits) - 1ULL);

      std::lock_guard lock(mutex_);
      nonce_fixed_ = fixed;
      nonce_mask_ = mask;
      extranonce2_hex_digits_ = (*en2_bytes_opt) * 2;
      if (active_job_.has_value()) {
        active_job_->nonce_fixed = nonce_fixed_;
        active_job_->nonce_mask = nonce_mask_;
        active_job_->extranonce2_hex_digits = extranonce2_hex_digits_;
        active_job_->generation =
            job_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      }
      return true;
    }

    if (method == "mining.challenge") {
      need(params.size() >= 2 &&
               params[0].kind_case() == google::protobuf::Value::kStringValue &&
               params[1].kind_case() == google::protobuf::Value::kStringValue,
           "mining.challenge requires [model_id_hex, nonce_hex]");
      const std::string &model_hex = params[0].string_value();
      const std::string &nonce_hex = params[1].string_value();
      (void)parse_hash32_hex(model_hex);
      const auto nonce_bytes = hex_decode_bytes(nonce_hex);
      need(nonce_bytes.size() == 8, "Challenge nonce must be 8 bytes (16 hex)");

      std::lock_guard lock(mutex_);
      pending_challenge_ = StratumChallenge{
          ++challenge_seq_, model_hex, nonce_hex,
          std::chrono::steady_clock::now()};
      std::cout << "{\"event\":\"STRATUM_CHALLENGE_RECEIVED\",\"model_id\":\""
                << model_hex.substr(0, 16) << "\",\"nonce\":\"" << nonce_hex
                << "\"}\n"
                << std::flush;
      return true;
    }

    if (method == "mining.notify") {
      need(params.size() >= 2 &&
               params[0].kind_case() == google::protobuf::Value::kStringValue,
           "mining.notify missing job_id");
      StratumPoolJob job;
      job.job_id = params[0].string_value();
      need(!job.job_id.empty(), "Empty Stratum job_id");
      job.received_at = std::chrono::steady_clock::now();

      if (params.size() >= 6 &&
          params[1].kind_case() == google::protobuf::Value::kStringValue &&
          params[5].kind_case() == google::protobuf::Value::kStringValue) {
        // Stratum v4 (H15): [job_id, header_hex, timestamp, daa_score, bits, window_seed_hex, clean_jobs?]
        job.protocol_version = 4;
        const auto header_bytes = hex_decode_bytes(params[1].string_value());
        need(!header_bytes.empty() &&
                 job.wire_header.ParseFromArray(
                     header_bytes.data(),
                     static_cast<int>(header_bytes.size())),
             "Invalid Stratum v4 protobuf RpcBlockHeader");
        job.header = consensus::header_from_rpc(job.wire_header);

        const auto ts_opt = value_to_u64(params[2]);
        const auto daa_opt = value_to_u64(params[3]);
        const auto bits_opt = value_to_u64(params[4]);
        need(ts_opt.has_value() && daa_opt.has_value() && bits_opt.has_value() &&
                 *bits_opt > 0 && *bits_opt <= 0xffffffffULL,
             "Invalid Stratum v4 timestamp/daa/bits");
        need(job.header.timestamp == *ts_opt &&
                 job.header.daa_score == *daa_opt &&
                 job.header.bits == static_cast<std::uint32_t>(*bits_opt),
             "Stratum v4 header fields mismatch notify params");
        job.timestamp = *ts_opt;
        job.daa_score = *daa_opt;
        job.bits = static_cast<std::uint32_t>(*bits_opt);
        job.has_bits = true;
        job.network_target = consensus::decode_compact_target(job.bits).little_endian;

        const std::string &seed_hex = params[5].string_value();
        if (!seed_hex.empty()) {
          job.window_seed = parse_hash32_hex(seed_hex);
          job.has_window_seed = true;
        }
        if (job.header.daa_score >= consensus::block_task_activation_daa &&
            job.header.task_root != Hash{}) {
          job.task_root = job.header.task_root;
          job.has_task_root = true;
        }
      } else {
        // Stratum v3 / v2 backward compatibility:
        // [job_id, header_or_u64s, timestamp, daa_score?, bits_or_task?]
        job.protocol_version = 3;
        if (params[1].kind_case() == google::protobuf::Value::kListValue) {
          const auto &words = params[1].list_value().values();
          need(words.size() == 4, "Expected 4 u64 words in notify header");
          for (int w = 0; w < 4; ++w) {
            const auto u = value_to_u64(words[w]);
            need(u.has_value(), "Invalid u64 word in notify header");
            for (int b = 0; b < 8; ++b) {
              job.pre_pow[static_cast<std::size_t>(w * 8 + b)] =
                  static_cast<std::uint8_t>((*u >> (8 * b)) & 0xffU);
            }
          }
          const auto ts_opt = value_to_u64(params[2]);
          need(ts_opt.has_value(), "Invalid notify timestamp");
          job.timestamp = *ts_opt;
        } else if (params[1].kind_case() ==
                   google::protobuf::Value::kStringValue) {
          const auto raw = hex_decode_bytes(params[1].string_value());
          need(raw.size() >= 32, "Short notify header hex");
          std::memcpy(job.pre_pow.data(), raw.data(), 32);
          if (params.size() >= 3) {
            if (const auto ts_opt = value_to_u64(params[2])) {
              job.timestamp = *ts_opt;
            }
          }
        } else {
          throw std::runtime_error("Unsupported mining.notify header format");
        }
        if (params.size() >= 4) {
          const auto daa_opt = value_to_u64(params[3]);
          need(daa_opt.has_value(), "Invalid notify DAA score");
          job.daa_score = *daa_opt;
        }
        if (params.size() >= 5) {
          if (params[4].kind_case() == google::protobuf::Value::kStringValue) {
            job.ai_task_json = params[4].string_value();
          } else if (const auto bits_opt = value_to_u64(params[4])) {
            need(*bits_opt > 0 && *bits_opt <= 0xffffffffULL, "Invalid bits");
            job.bits = static_cast<std::uint32_t>(*bits_opt);
            job.has_bits = true;
            job.network_target = consensus::decode_compact_target(job.bits).little_endian;
          }
        }
      }

      std::lock_guard lock(mutex_);
      job.difficulty = current_difficulty_;
      job.share_target = current_share_target_;
      job.nonce_fixed = nonce_fixed_;
      job.nonce_mask = nonce_mask_;
      job.extranonce2_hex_digits = extranonce2_hex_digits_;

      if (job.protocol_version == 4) {
        if (job.header.daa_score >= consensus::block_task_activation_daa) {
          if (!job.has_task_root && job.has_window_seed) {
            for (const auto &[s, r] : window_task_roots_) {
              if (s == job.window_seed) {
                job.task_root = r;
                job.has_task_root = true;
                job.header.task_root = r;
                break;
              }
            }
          }
        }
        job.pre_pow = consensus::pre_pow_hash(job.header);
      }

      if (job.has_bits &&
          compare_target_u256(job.network_target, job.share_target) > 0) {
        job.effective_target = job.network_target;
      } else {
        job.effective_target = job.share_target;
      }

      job.generation =
          job_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      active_job_ = job;
      ++jobs_received_;
      state_ = ConnectionState::Ready;
      last_error_.clear();
      if (job_ids_seen_.size() >= kMaxSeenJobs) {
        job_ids_seen_.clear();
        submitted_nonces_by_job_.clear();
      }
      job_ids_seen_[job.job_id] = job.generation;

      std::cout << "{\"event\":\"STRATUM_JOB_NOTIFY\",\"job_id\":\""
                << json_escape(job.job_id) << "\",\"version\":"
                << job.protocol_version << ",\"daa\":" << job.daa_score
                << ",\"generation\":" << job.generation
                << ",\"has_window_seed\":"
                << (job.has_window_seed ? "true" : "false")
                << ",\"has_task_root\":"
                << (job.has_task_root ? "true" : "false") << "}\n"
                << std::flush;
      return true;
    }

    return true;
  }

  // Handle JSON-RPC response (id, result, error)
  const auto id_it = fields.find("id");
  if (id_it == fields.end()) {
    return false;
  }
  const auto id_opt = value_to_u64(id_it->second);
  if (!id_opt.has_value()) {
    return false;
  }
  const std::uint64_t rpc_id = *id_opt;
  const auto now = std::chrono::steady_clock::now();

  const auto err_it = fields.find("error");
  const bool has_error =
      err_it != fields.end() &&
      err_it->second.kind_case() != google::protobuf::Value::kNullValue;
  const auto res_it = fields.find("result");

  std::lock_guard lock(mutex_);
  if (rpc_id == subscribe_rpc_id_) {
    ping_ms_ = std::chrono::duration<double, std::milli>(now - subscribe_sent_at_)
                   .count();
    if (has_error) {
      state_ = ConnectionState::Error;
      record_error_locked("mining.subscribe rejected by pool");
      return true;
    }
    if (res_it != fields.end() &&
        res_it->second.kind_case() == google::protobuf::Value::kListValue) {
      const auto &arr = res_it->second.list_value().values();
      if (arr.size() >= 1 &&
          arr[0].kind_case() == google::protobuf::Value::kListValue) {
        for (const auto &sub : arr[0].list_value().values()) {
          if (sub.kind_case() == google::protobuf::Value::kListValue) {
            const auto &pair = sub.list_value().values();
            if (pair.size() >= 2 &&
                pair[0].kind_case() == google::protobuf::Value::kStringValue &&
                pair[1].kind_case() == google::protobuf::Value::kStringValue) {
              if (pair[0].string_value() == "keryx.proof_encoding" &&
                  pair[1].string_value() == "b64") {
                pool_supports_b64_proof_ = true;
                std::cout << "{\"event\":\"STRATUM_CAPABILITY\",\"capability\":\"keryx.proof_encoding\",\"value\":\"b64\"}\n"
                          << std::flush;
              }
            }
          }
        }
      }
      if (arr.size() >= 2 &&
          arr[1].kind_case() == google::protobuf::Value::kStringValue &&
          !arr[1].string_value().empty()) {
        const auto en_bytes = hex_decode_bytes(arr[1].string_value());
        std::size_t en2_bytes = 8 - std::min<std::size_t>(8, en_bytes.size());
        if (arr.size() >= 3) {
          if (const auto sz = value_to_u64(arr[2])) {
            en2_bytes = *sz;
          }
        }
        if (en_bytes.size() + en2_bytes <= 8) {
          std::uint64_t fixed = 0;
          for (std::uint8_t b : en_bytes) {
            fixed = (fixed << 8) | static_cast<std::uint64_t>(b);
          }
          const std::size_t en2_bits = en2_bytes * 8;
          fixed = (en2_bits < 64) ? (fixed << en2_bits) : 0ULL;
          nonce_fixed_ = fixed;
          nonce_mask_ = (en2_bits >= 64) ? 0xffffffffffffffffULL
                                         : ((1ULL << en2_bits) - 1ULL);
          extranonce2_hex_digits_ = en2_bytes * 2;
        }
      }
    }
    if (state_ == ConnectionState::Connected ||
        state_ == ConnectionState::Connecting) {
      state_ = ConnectionState::Subscribed;
    }
    return true;
  }

  if (rpc_id == authorize_rpc_id_) {
    ping_ms_ = std::chrono::duration<double, std::milli>(now - authorize_sent_at_)
                   .count();
    const bool accepted =
        !has_error && res_it != fields.end() &&
        res_it->second.kind_case() == google::protobuf::Value::kBoolValue &&
        res_it->second.bool_value();
    if (!accepted) {
      state_ = ConnectionState::Error;
      record_error_locked("Pool authorization rejected for worker " +
                          config_.worker);
      return true;
    }
    state_ = ConnectionState::Ready;
    last_error_.clear();
    std::cout << "{\"event\":\"STRATUM_AUTHORIZED\",\"worker\":\""
              << json_escape(config_.worker) << "\",\"ping_ms\":" << ping_ms_
              << "}\n"
              << std::flush;
    return true;
  }

  const auto pending_it = pending_shares_.find(rpc_id);
  if (pending_it != pending_shares_.end()) {
    const PendingShareRecord rec = pending_it->second;
    pending_shares_.erase(pending_it);
    ping_ms_ =
        std::chrono::duration<double, std::milli>(now - rec.sent_at).count();

    const bool accepted =
        !has_error && res_it != fields.end() &&
        res_it->second.kind_case() == google::protobuf::Value::kBoolValue &&
        res_it->second.bool_value();
    const std::string block_hash_hex =
        hex_encode_bytes(std::span<const std::uint8_t>(rec.block_hash.data(), 32));

    if (accepted) {
      ++accepted_shares_;
      if (rec.satisfies_network_target) {
        ++blocks_submitted_;
      }
      last_share_status_ = "ACCEPTED";
      last_error_.clear();
      std::cout << "{\"event\":\"LIVE_SUBMIT_RESULT\",\"source\":\"CUDA\","
                << "\"mode\":\"POOL\",\"state\":\"RPC_ACKNOWLEDGED\","
                << "\"job_id\":\"" << json_escape(rec.job_id) << "\","
                << "\"nonce\":\"" << rec.nonce << "\","
                << "\"hash\":\"" << block_hash_hex << "\","
                << "\"ping_ms\":" << ping_ms_ << "}\n"
                << "{\"event\":\"STRATUM_SHARE_RESULT\",\"status\":\"ACCEPTED\","
                << "\"job_id\":\"" << json_escape(rec.job_id) << "\","
                << "\"nonce\":\"" << rec.nonce << "\","
                << "\"ping_ms\":" << ping_ms_ << "}\n"
                << std::flush;
      return true;
    }

    int err_code = static_cast<int>(ShareRejectCode::Other);
    std::string err_msg = "Share rejected";
    if (has_error) {
      if (err_it->second.kind_case() == google::protobuf::Value::kListValue) {
        const auto &earr = err_it->second.list_value().values();
        if (earr.size() >= 1) {
          if (const auto c = value_to_u64(earr[0])) {
            err_code = static_cast<int>(*c);
          }
        }
        if (earr.size() >= 2 &&
            earr[1].kind_case() == google::protobuf::Value::kStringValue) {
          err_msg = earr[1].string_value();
        }
      } else if (err_it->second.kind_case() ==
                 google::protobuf::Value::kStructValue) {
        const auto &ef = err_it->second.struct_value().fields();
        if (const auto cit = ef.find("code"); cit != ef.end()) {
          if (const auto c = value_to_u64(cit->second)) {
            err_code = static_cast<int>(*c);
          }
        }
        if (const auto mit = ef.find("message");
            mit != ef.end() &&
            mit->second.kind_case() == google::protobuf::Value::kStringValue) {
          err_msg = mit->second.string_value();
        }
      } else if (err_it->second.kind_case() ==
                 google::protobuf::Value::kStringValue) {
        err_msg = err_it->second.string_value();
      }
    }

    ++rejected_shares_;
    if (rec.was_b64 && pool_supports_b64_proof_) {
      pool_supports_b64_proof_ = false;
      std::cout << "{\"event\":\"STRATUM_CAPABILITY_FALLBACK\",\"capability\":\"keryx.proof_encoding\",\"fallback\":\"hex\"}\n"
                << std::flush;
    }
    std::string lower_msg = err_msg;
    for (char &c : lower_msg)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (err_code == static_cast<int>(ShareRejectCode::JobNotFoundStale) ||
        lower_msg.find("stale") != std::string::npos ||
        lower_msg.find("missing job") != std::string::npos ||
        lower_msg.find("job does not exist") != std::string::npos) {
      ++stale_shares_;
      last_share_status_ = "REJECTED_STALE";
    } else if (err_code == static_cast<int>(ShareRejectCode::DuplicateShare) ||
               lower_msg.find("duplicate") != std::string::npos) {
      ++duplicate_shares_;
      last_share_status_ = "REJECTED_DUPLICATE";
    } else if (err_code == static_cast<int>(ShareRejectCode::LowDifficulty) ||
               lower_msg.find("low difficulty") != std::string::npos) {
      ++low_diff_shares_;
      last_share_status_ = "REJECTED_LOW_DIFF";
    } else if (err_code ==
               static_cast<int>(ShareRejectCode::TaskRootRequired)) {
      last_share_status_ = "REJECTED_TASK_ROOT_REQUIRED";
    } else {
      last_share_status_ = "REJECTED";
    }

    last_error_ =
        "Share rejected (" + std::to_string(err_code) + "): " + err_msg;
    std::cout << "{\"event\":\"LIVE_SUBMIT_RESULT\",\"source\":\"CUDA\","
              << "\"mode\":\"POOL\",\"state\":\"RPC_REJECTED\","
              << "\"job_id\":\"" << json_escape(rec.job_id) << "\","
              << "\"nonce\":\"" << rec.nonce << "\","
              << "\"hash\":\"" << block_hash_hex << "\","
              << "\"reject_reason\":\"" << json_escape(err_msg) << "\","
              << "\"code\":" << err_code << "}\n"
              << "{\"event\":\"STRATUM_SHARE_RESULT\",\"status\":\"REJECTED\","
              << "\"job_id\":\"" << json_escape(rec.job_id) << "\","
              << "\"nonce\":\"" << rec.nonce << "\","
              << "\"code\":" << err_code << ",\"reason\":\""
              << json_escape(err_msg) << "\"}\n"
              << std::flush;
    return true;
  }

  return true;
}

std::optional<StratumPoolJob> StratumV4Client::current_job() const {
  std::lock_guard lock(mutex_);
  return active_job_;
}

bool StratumV4Client::is_job_current(std::uint64_t generation,
                                     std::string_view job_id) const {
  if (job_generation_.load(std::memory_order_acquire) != generation) {
    return false;
  }
  std::lock_guard lock(mutex_);
  return active_job_.has_value() && active_job_->generation == generation &&
         active_job_->job_id == job_id;
}

std::optional<Hash>
StratumV4Client::lookup_window_task_root(const Hash &window_seed) const {
  std::lock_guard lock(mutex_);
  for (const auto &[s, r] : window_task_roots_) {
    if (s == window_seed) {
      return r;
    }
  }
  return std::nullopt;
}

void StratumV4Client::commit_window_task_root(const Hash &window_seed,
                                              const Hash &task_root) {
  need(window_seed != Hash{} && task_root != Hash{},
       "Window seed and task root must be non-zero 32-byte hashes");
  std::lock_guard lock(mutex_);
  bool found = false;
  for (auto &[s, r] : window_task_roots_) {
    if (s == window_seed) {
      r = task_root;
      found = true;
      break;
    }
  }
  if (!found) {
    if (window_task_roots_.size() >= kMaxWindowSeedCache) {
      window_task_roots_.pop_front();
    }
    window_task_roots_.emplace_back(window_seed, task_root);
  }
  if (active_job_.has_value() && active_job_->protocol_version == 4 &&
      active_job_->has_window_seed && active_job_->window_seed == window_seed) {
    active_job_->task_root = task_root;
    if (active_job_->header.daa_score >= consensus::block_task_activation_daa) {
      active_job_->has_task_root = true;
      active_job_->header.task_root = task_root;
      active_job_->pre_pow = consensus::hash_header_override(
          active_job_->header, 0, 0, false, false, true);
    }
  }
}

std::optional<StratumChallenge> StratumV4Client::take_pending_challenge() {
  std::lock_guard lock(mutex_);
  std::optional<StratumChallenge> out = std::move(pending_challenge_);
  pending_challenge_.reset();
  return out;
}

bool StratumV4Client::submit_challenge_response(std::string_view model_id_hex,
                                                std::string_view nonce_hex,
                                                std::string_view result_text) {
  need(!model_id_hex.empty() && !nonce_hex.empty() && !result_text.empty(),
       "Challenge response fields must be non-empty");
  std::lock_guard lock(mutex_);
  const std::uint64_t rpc_id = next_rpc_id_++;
  const std::string msg =
      "{\"id\":" + std::to_string(rpc_id) +
      ",\"method\":\"mining.challenge_response\",\"params\":[\"" +
      json_escape(model_id_hex) + "\",\"" + json_escape(nonce_hex) + "\",\"" +
      json_escape(result_text) + "\"]}";
  if (!send_line_locked(msg)) {
    record_error_locked("Failed sending mining.challenge_response");
    return false;
  }
  ++challenges_passed_;
  std::cout << "{\"event\":\"STRATUM_CHALLENGE_RESPONSE\",\"nonce\":\""
            << json_escape(nonce_hex) << "\",\"bytes\":" << result_text.size()
            << "}\n"
            << std::flush;
  return true;
}

void StratumV4Client::record_local_stale_drop(std::string_view job_id,
                                              std::uint64_t nonce,
                                              std::string_view stage) {
  std::lock_guard lock(mutex_);
  ++stale_shares_;
  last_share_status_ = "STALE_DROPPED";
  std::cout << "{\"event\":\"STRATUM_STALE_DROP\",\"job_id\":\""
            << json_escape(job_id) << "\",\"nonce\":\"" << nonce
            << "\",\"stage\":\"" << json_escape(stage) << "\"}\n"
            << std::flush;
}

std::optional<std::uint64_t>
StratumV4Client::submit_share(const VerifiedPoolShare &share) {
  std::lock_guard lock(mutex_);
  if (!active_job_.has_value() ||
      active_job_->generation != share.job_generation ||
      active_job_->job_id != share.job_id) {
    ++stale_shares_;
    last_share_status_ = "STALE_BEFORE_SUBMIT";
    std::cout << "{\"event\":\"STRATUM_STALE_DROP\",\"job_id\":\""
              << json_escape(share.job_id) << "\",\"nonce\":\"" << share.nonce
              << "\",\"stage\":\"PRE_SUBMIT\"}\n"
              << std::flush;
    return std::nullopt;
  }

  auto &seen_nonces = submitted_nonces_by_job_[share.job_id];
  if (seen_nonces.find(share.nonce) != seen_nonces.end()) {
    ++duplicate_shares_;
    last_share_status_ = "DUPLICATE_DROPPED_LOCAL";
    std::cout << "{\"event\":\"STRATUM_DUPLICATE_DROP\",\"job_id\":\""
              << json_escape(share.job_id) << "\",\"nonce\":\"" << share.nonce
              << "\",\"stage\":\"PRE_SUBMIT\"}\n"
              << std::flush;
    return std::nullopt;
  }
  seen_nonces.insert(share.nonce);

  need((share.nonce & ~active_job_->nonce_mask) == active_job_->nonce_fixed,
       "Share nonce violates extranonce assignment");
  need(compare_target_u256(share.pow_value, active_job_->effective_target) <= 0,
       "Share pow_value does not satisfy effective target");
  need(share.pom_proof_borsh.size() >= 73,
       "Share PoM proof shorter than 73-byte Borsh prefix");

  const std::string opoi_tag =
      share.opoi_tag_hex.empty() ? compute_stratum_opoi_tag(share.nonce)
                                 : share.opoi_tag_hex;
  need(opoi_tag.size() == 16 &&
           opoi_tag == compute_stratum_opoi_tag(share.nonce),
       "Share OPoI tag verification failed");

  const std::string nonce_hex = u64_hex_padded(share.nonce, 16);
  std::string proof_field;
  bool was_b64 = false;
  if (pool_supports_b64_proof_) {
    proof_field = "b64:" + base64_encode(share.pom_proof_borsh);
    was_b64 = true;
  } else {
    proof_field = hex_encode_bytes(share.pom_proof_borsh);
  }

  const bool send_task_root =
      (share.protocol_version == 4 &&
       share.daa_score >= consensus::block_task_activation_daa);
  Hash committed_task_root =
      share.has_task_root ? share.task_root : active_job_->task_root;
  if (send_task_root) {
    need(committed_task_root != Hash{},
         "Stratum v4 share requires non-zero 32-byte task_root");
  }
  const std::string task_root_hex =
      send_task_root
          ? hex_encode_bytes(std::span<const std::uint8_t>(
                committed_task_root.data(), committed_task_root.size()))
          : std::string{};

  const std::uint64_t rpc_id = next_rpc_id_++;
  std::ostringstream oss;
  oss << "{\"id\":" << rpc_id << ",\"method\":\"mining.submit\",\"params\":[\""
      << json_escape(config_.worker) << "\",\"" << json_escape(share.job_id)
      << "\",\"" << nonce_hex << "\",\"" << opoi_tag << "\",\""
      << json_escape(share.ipfs_cid) << "\",\"" << proof_field << "\"";
  if (send_task_root) {
    oss << ",\"" << task_root_hex << "\"";
  }
  oss << "]}";

  if (!send_line_locked(oss.str())) {
    record_error_locked("Failed transmitting mining.submit to pool");
    return std::nullopt;
  }

  const bool satisfies_net =
      active_job_->has_bits &&
      compare_target_u256(share.pow_value, active_job_->network_target) <= 0;

  PendingShareRecord rec{};
  rec.rpc_id = rpc_id;
  rec.job_generation = share.job_generation;
  rec.job_id = share.job_id;
  rec.nonce = share.nonce;
  rec.pow_value = share.pow_value;
  rec.block_hash = share.block_hash;
  rec.has_task_root = send_task_root;
  rec.task_root = committed_task_root;
  rec.satisfies_network_target = satisfies_net;
  rec.was_b64 = was_b64;
  rec.sent_at = std::chrono::steady_clock::now();
  pending_shares_[rpc_id] = rec;

  const std::string block_hash_hex =
      hex_encode_bytes(std::span<const std::uint8_t>(share.block_hash.data(), 32));
  std::cout << "{\"event\":\"LIVE_SUBMIT_RESULT\",\"source\":\"CUDA\","
            << "\"mode\":\"POOL\",\"state\":\"SUBMIT_SENT\","
            << "\"job_id\":\"" << json_escape(share.job_id) << "\","
            << "\"nonce\":\"" << share.nonce << "\","
            << "\"hash\":\"" << block_hash_hex << "\","
            << "\"fields\":" << (send_task_root ? 7 : 6) << "}\n"
            << std::flush;
  return rpc_id;
}

StratumTelemetrySnapshot StratumV4Client::telemetry_snapshot() const {
  std::lock_guard lock(mutex_);
  StratumTelemetrySnapshot snap;
  snap.mode = "pool";
  snap.connected = (state_ == ConnectionState::Connected ||
                    state_ == ConnectionState::Subscribed ||
                    state_ == ConnectionState::Authorized ||
                    state_ == ConnectionState::Ready);
  snap.state = std::string(to_string(state_));
  snap.endpoint = endpoint_.raw_url;
  snap.worker = config_.worker;
  snap.ping_ms = ping_ms_;
  snap.difficulty = current_difficulty_;
  snap.accepted_shares = accepted_shares_;
  snap.rejected_shares = rejected_shares_;
  snap.stale_shares = stale_shares_;
  snap.duplicate_shares = duplicate_shares_;
  snap.low_diff_shares = low_diff_shares_;
  snap.blocks_submitted = blocks_submitted_;
  snap.pending_shares = pending_shares_.size();
  snap.jobs_received = jobs_received_;
  snap.reconnect_count = reconnect_count_;
  snap.challenges_passed = challenges_passed_;
  if (active_job_.has_value()) {
    snap.active_job_id = active_job_->job_id;
    snap.active_daa_score = active_job_->daa_score;
    if (active_job_->has_window_seed) {
      snap.window_seed_hex = hex_encode_bytes(
          std::span<const std::uint8_t>(active_job_->window_seed.data(), 32));
    }
    if (active_job_->has_task_root) {
      snap.task_root_hex = hex_encode_bytes(
          std::span<const std::uint8_t>(active_job_->task_root.data(), 32));
    }
  }
  snap.last_error = last_error_;
  snap.last_share_status = last_share_status_;
  return snap;
}

} // namespace keryx::nexus::stratum
