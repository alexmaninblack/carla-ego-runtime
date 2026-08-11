#include "carla_ego_runtime/vss.hpp"

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>

namespace carla_ego_runtime {
namespace {

constexpr std::string_view kProfileVersion = "0.1";

void Add(VssSnapshot &snapshot, std::string path, VssValue value) {
  snapshot.data_points.push_back(
      {std::move(path), std::move(value), snapshot.timestamp});
}

}  // namespace

std::string FormatIso8601Utc(
    std::chrono::system_clock::time_point timestamp_utc) {
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          timestamp_utc.time_since_epoch());
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
  auto remainder = milliseconds -
                   std::chrono::duration_cast<std::chrono::milliseconds>(seconds);
  if (remainder.count() < 0) {
    seconds -= std::chrono::seconds(1);
    remainder += std::chrono::milliseconds(1000);
  }

  const std::time_t time = static_cast<std::time_t>(seconds.count());
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << remainder.count() << 'Z';
  return output.str();
}

VssSnapshot ProjectToVss(const NormalizedVehicleState &state) {
  VssSnapshot snapshot;
  snapshot.frame_id = state.frame_id;
  snapshot.simulation_time_s = state.simulation_time_s;
  snapshot.timestamp = FormatIso8601Utc(state.timestamp_utc);

  Add(snapshot, "Vehicle.Speed", state.speed_mps * 3.6);
  Add(snapshot, "Vehicle.Acceleration.Longitudinal",
      state.acceleration_iso_mps2.x);
  Add(snapshot, "Vehicle.Acceleration.Lateral",
      state.acceleration_iso_mps2.y);
  Add(snapshot, "Vehicle.Acceleration.Vertical",
      state.acceleration_iso_mps2.z);
  Add(snapshot, "Vehicle.Chassis.Accelerator.PedalPosition",
      static_cast<std::uint64_t>(std::lround(state.throttle_command * 100.0)));
  Add(snapshot, "Vehicle.Chassis.Brake.PedalPosition",
      static_cast<std::uint64_t>(std::lround(state.brake_command * 100.0)));
  if (state.equivalent_front_axle_angle_iso_deg.has_value()) {
    Add(snapshot, "Vehicle.Chassis.Axle.Row1.SteeringAngle",
        *state.equivalent_front_axle_angle_iso_deg);
  }
  Add(snapshot, "Vehicle.Powertrain.Transmission.CurrentGear",
      static_cast<std::int64_t>(state.gear));
  if (state.engine_rpm.has_value()) {
    Add(snapshot, "Vehicle.Powertrain.CombustionEngine.Speed",
        *state.engine_rpm);
  }
  Add(snapshot, "Vehicle.CarlaSimulation.ProfileVersion",
      std::string(kProfileVersion));
  Add(snapshot, "Vehicle.CarlaSimulation.RunId", state.run_id);
  Add(snapshot, "Vehicle.CarlaSimulation.EgoVehicleId",
      state.ego_vehicle_id);
  Add(snapshot, "Vehicle.CarlaSimulation.FrameId", state.frame_id);
  Add(snapshot, "Vehicle.CarlaSimulation.SimulationTime",
      state.simulation_time_s);
  return snapshot;
}

bool LatestVssSignalStore::Publish(VssSnapshot snapshot) {
  std::scoped_lock lock(mutex_);
  if (latest_.has_value() && snapshot.frame_id <= latest_->frame_id) {
    return false;
  }
  latest_ = std::move(snapshot);
  ++publish_count_;
  return true;
}

std::optional<VssSnapshot> LatestVssSignalStore::Latest() const {
  std::scoped_lock lock(mutex_);
  return latest_;
}

std::uint64_t LatestVssSignalStore::publish_count() const {
  std::scoped_lock lock(mutex_);
  return publish_count_;
}

}  // namespace carla_ego_runtime
