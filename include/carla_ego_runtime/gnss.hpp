#pragma once

#include "carla_ego_runtime/vehicle_state.hpp"

#include <cstdint>
#include <mutex>
#include <optional>

namespace carla_ego_runtime {

struct CarlaGnssSample {
  std::uint64_t frame_id = 0;
  double simulation_time_s = 0.0;
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_m = 0.0;
};

struct NormalizedGnssFix {
  std::uint64_t source_frame_id = 0;
  double source_simulation_time_s = 0.0;
  std::chrono::system_clock::time_point timestamp_utc;
  double latitude_deg = 0.0;
  double longitude_deg = 0.0;
  double altitude_m = 0.0;
};

NormalizedGnssFix NormalizeGnssSample(const CarlaGnssSample &sample,
                                     const SimulationClockAnchor &clock_anchor);

// Thread-safe handoff between CARLA's sensor callback and the synchronous
// frame assembler. It retains one fix, rejects non-increasing source frames,
// and returns data only when it is attributable to and fresh for the requested
// vehicle-state frame.
class LatestGnssFixStore {
 public:
  bool Publish(CarlaGnssSample sample);
  std::optional<CarlaGnssSample> LatestFor(
      std::uint64_t vehicle_frame_id, double vehicle_simulation_time_s,
      double max_age_seconds) const;
  std::uint64_t publish_count() const;
  std::uint64_t rejected_count() const;

 private:
  mutable std::mutex mutex_;
  std::optional<CarlaGnssSample> latest_;
  std::uint64_t publish_count_ = 0;
  std::uint64_t rejected_count_ = 0;
};

}  // namespace carla_ego_runtime
