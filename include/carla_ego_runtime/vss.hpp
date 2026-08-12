#pragma once

#include "carla_ego_runtime/vehicle_state.hpp"
#include "carla_ego_runtime/gnss.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace carla_ego_runtime {

using VssValue =
    std::variant<std::string, double, std::int64_t, std::uint64_t>;

struct VssDataPoint {
  std::string path;
  VssValue value;
  std::string timestamp;
};

struct VssSnapshot {
  std::uint64_t frame_id = 0;
  double simulation_time_s = 0.0;
  std::string timestamp;
  std::vector<VssDataPoint> data_points;
};

std::string FormatIso8601Utc(
    std::chrono::system_clock::time_point timestamp_utc);
VssSnapshot ProjectToVss(
    const NormalizedVehicleState &state,
    const std::optional<NormalizedGnssFix> &gnss_fix = std::nullopt);

// A bounded, thread-safe last-value store. Publishing a duplicate or older
// frame is rejected, so every accepted frame contributes exactly one update.
class LatestVssSignalStore {
 public:
  bool Publish(VssSnapshot snapshot);
  std::optional<VssSnapshot> Latest() const;
  std::uint64_t publish_count() const;

 private:
  mutable std::mutex mutex_;
  std::optional<VssSnapshot> latest_;
  std::uint64_t publish_count_ = 0;
};

}  // namespace carla_ego_runtime
