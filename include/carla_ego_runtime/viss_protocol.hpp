#pragma once

#include "carla_ego_runtime/vss.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carla_ego_runtime {

struct VissProtocolLimits {
  std::size_t max_subscriptions = 16;
  std::chrono::milliseconds minimum_period{50};
  std::chrono::milliseconds maximum_period{60000};
};

struct VissProtocolMetrics {
  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::uint64_t subscription_events = 0;
  std::uint64_t coalesced_intervals = 0;
};

struct VissResponse {
  std::string payload;
  bool is_error = false;
};

// Protocol state is intentionally scoped to one WebSocket connection. VISS
// subscription identifiers therefore cannot leak across reconnects.
class VissSessionProtocol {
public:
  explicit VissSessionProtocol(VissProtocolLimits limits = {});
  ~VissSessionProtocol();

  VissSessionProtocol(const VissSessionProtocol &) = delete;
  VissSessionProtocol &operator=(const VissSessionProtocol &) = delete;
  VissSessionProtocol(VissSessionProtocol &&) noexcept;
  VissSessionProtocol &operator=(VissSessionProtocol &&) noexcept;

  VissResponse
  HandleRequest(std::string_view request,
                const LatestVssSignalStore &signal_store,
                std::chrono::system_clock::time_point response_time =
                    std::chrono::system_clock::now(),
                std::chrono::steady_clock::time_point schedule_time =
                    std::chrono::steady_clock::now());

  std::vector<std::string> CollectDueSubscriptionEvents(
      const LatestVssSignalStore &signal_store,
      std::chrono::system_clock::time_point response_time =
          std::chrono::system_clock::now(),
      std::chrono::steady_clock::time_point schedule_time =
          std::chrono::steady_clock::now());

  VissProtocolMetrics metrics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace carla_ego_runtime
