#pragma once

#include "carla_ego_runtime/viss_access.hpp"
#include "carla_ego_runtime/vss.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace carla_ego_runtime {

bool IsQmAdvisoryRequestPath(std::string_view path);
bool IsQmAdvisoryAvailabilityPath(std::string_view path);
bool IsQmAdvisoryPath(std::string_view path);

struct QmAdvisoryResult {
  bool accepted = false;
  std::string reason;
};

// Gateway-owned, two-endpoint current state. Shared by authenticated VISS
// sessions; never accepts motion controls or caller-selected authorization.
class QmAdvisoryGateway {
public:
  explicit QmAdvisoryGateway(std::chrono::system_clock::time_point started_at =
                                std::chrono::system_clock::now());
  ~QmAdvisoryGateway();
  QmAdvisoryResult Handle(VissSessionAccess access, std::string_view path,
                         std::string_view raw,
                         std::chrono::system_clock::time_point now,
                         std::chrono::steady_clock::time_point monotonic_now);
  std::vector<VssDataPoint> Snapshot(
      std::chrono::system_clock::time_point now,
      std::chrono::steady_clock::time_point monotonic_now);
  void ResetAssignment();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace carla_ego_runtime
