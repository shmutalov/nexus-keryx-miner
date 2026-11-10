#pragma once
#include "keryx/nexus/ai_response_service.hpp"
#include <filesystem>
#include <functional>
#include <mutex>

namespace keryx::nexus::ai {
// Synchronous worker-side adapter. Its caller owns GPU scheduling; the Stratum
// I/O thread only queues bounded requests and sends these unsigned results.
class PoolAiService {
public:
  using Hash = std::array<std::uint8_t, 32>;
  struct Capture {
    AiTracedAnswer answer;
    std::vector<std::uint8_t> record;
  };
  using Infer =
      std::function<Capture(std::string_view, std::uint32_t, const Hash &)>;
  using Open = std::function<std::vector<std::uint8_t>(
      std::span<const std::uint8_t>, const Hash &, const Hash &, const Hash &,
      std::span<const std::uint64_t>, std::span<const std::uint8_t>)>;
  PoolAiService(std::filesystem::path, const Hash &, Infer, Open);
  // Returns method+params, never a signed transaction or a share receipt.
  std::string handle(std::string_view pool_scope, std::string_view worker,
                     std::string_view wire);

private:
  std::filesystem::path root_;
  Hash model_;
  Infer infer_;
  Open open_;
  std::mutex mutex_;
};
} // namespace keryx::nexus::ai
