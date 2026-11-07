#include "keryx/nexus/h15_audit_client.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace keryx::nexus::ai;

int main() {
  int passed = 0;

  // 1. Zero obligations opening [0xB0, 0x00]
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x00};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(s.safe);
    assert(s.reason.empty());
    const auto pieces = H15AuditClient::unbundle(b);
    assert(pieces.empty());
    ++passed;
  }

  // 2. Zero obligations response [0xB0, 0x00]
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x00};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "RESPONSES");
    assert(s.safe);
    assert(s.reason.empty());
    ++passed;
  }

  // 3. Single obligation bundled [0xB0, 0x01, len=4, 't','e','s','t']
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x01, 0x04, 0x00, 0x00, 0x00, 't', 'e', 's', 't'};
    const auto so = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!so.safe);
    assert(so.reason == "H15_OPENINGS_DUE_FOR_SUBMISSION");

    const auto sr = H15AuditClient::evaluate_obligation_bundle(b, "RESPONSES");
    assert(!sr.safe);
    assert(sr.reason == "H15_RESPONSES_DUE_FOR_SUBMISSION");

    const auto pieces = H15AuditClient::unbundle(b);
    assert(pieces.size() == 1);
    assert((pieces[0] == std::vector<std::uint8_t>{'t', 'e', 's', 't'}));
    ++passed;
  }

  // 4. Single obligation bare (no 0xB0 prefix)
  {
    const std::vector<std::uint8_t> b = {'b', 'a', 'r', 'e', '_', 'b', 'o', 'd', 'y'};
    const auto so = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!so.safe);
    assert(so.reason == "H15_OPENINGS_DUE_FOR_SUBMISSION");

    const auto sr = H15AuditClient::evaluate_obligation_bundle(b, "RESPONSES");
    assert(!sr.safe);
    assert(sr.reason == "H15_RESPONSES_DUE_FOR_SUBMISSION");

    const auto pieces = H15AuditClient::unbundle(b);
    assert(pieces.size() == 1);
    assert(pieces[0] == b);
    ++passed;
  }

  // 5. Multiple obligations bundled [0xB0, 0x02, piece1(3), piece2(2)]
  {
    const std::vector<std::uint8_t> b = {
        0xb0, 0x02,
        0x03, 0x00, 0x00, 0x00, 'o', 'p', '1',
        0x02, 0x00, 0x00, 0x00, 'o', '2'
    };
    const auto so = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!so.safe);
    assert(so.reason == "H15_OPENINGS_DUE_FOR_SUBMISSION");

    const auto sr = H15AuditClient::evaluate_obligation_bundle(b, "RESPONSES");
    assert(!sr.safe);
    assert(sr.reason == "H15_RESPONSES_DUE_FOR_SUBMISSION");

    const auto pieces = H15AuditClient::unbundle(b);
    assert(pieces.size() == 2);
    assert((pieces[0] == std::vector<std::uint8_t>{'o', 'p', '1'}));
    assert((pieces[1] == std::vector<std::uint8_t>{'o', '2'}));
    ++passed;
  }

  // 6. Unexpected empty buffer (fail-closed)
  {
    const std::vector<std::uint8_t> empty;
    const auto s1 = H15AuditClient::evaluate_obligation_bundle(empty, "OPENINGS");
    assert(!s1.safe);
    assert(s1.reason == "H15_OPENINGS_BUNDLE_INVALID: BUFFER_EMPTY");

    const auto s2 = H15AuditClient::evaluate_obligation_bundle(std::span<const std::uint8_t>{}, "RESPONSES");
    assert(!s2.safe);
    assert(s2.reason == "H15_RESPONSES_BUNDLE_INVALID: BUFFER_EMPTY");
    ++passed;
  }

  // 7. Truncated bundle header [0xB0] (missing count)
  {
    const std::vector<std::uint8_t> b = {0xb0};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!s.safe);
    assert(s.reason.find("H15_OPENINGS_BUNDLE_CORRUPT: Truncated audit bundle") != std::string::npos);
    ++passed;
  }

  // 8. Truncated length prefix (count=1, only 2 bytes for length)
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x01, 0x04, 0x00};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!s.safe);
    assert(s.reason.find("H15_OPENINGS_BUNDLE_CORRUPT: Truncated audit length") != std::string::npos);
    ++passed;
  }

  // 9. Truncated payload (declares 6 bytes, gives 2)
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x01, 0x06, 0x00, 0x00, 0x00, 'x', 'y'};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!s.safe);
    assert(s.reason.find("H15_OPENINGS_BUNDLE_CORRUPT: Audit piece bounds") != std::string::npos);
    ++passed;
  }

  // 10. Piece bounds exceeded (> 112KB + 96)
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x01, 0x00, 0x00, 0x05, 0x00}; // ~320KB
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!s.safe);
    assert(s.reason.find("H15_OPENINGS_BUNDLE_CORRUPT: Audit piece bounds") != std::string::npos);
    ++passed;
  }

  // 11. Trailing audit data after zero obligations [0xB0, 0x00, 0xFF]
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x00, 0xff};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "OPENINGS");
    assert(!s.safe);
    assert(s.reason.find("H15_OPENINGS_BUNDLE_CORRUPT: Trailing audit data") != std::string::npos);
    ++passed;
  }

  // 12. Trailing audit data after valid pieces
  {
    const std::vector<std::uint8_t> b = {0xb0, 0x01, 0x01, 0x00, 0x00, 0x00, 'k', 0x99};
    const auto s = H15AuditClient::evaluate_obligation_bundle(b, "RESPONSES");
    assert(!s.safe);
    assert(s.reason.find("H15_RESPONSES_BUNDLE_CORRUPT: Trailing audit data") != std::string::npos);
    ++passed;
  }

  std::cout << "H15_BUNDLE_CANONICAL_TESTS=PASS cases=" << passed
            << " zero_obligations_unbundled=PASS obligations_due_gated=PASS fail_closed=PASS\n";
  return 0;
}
