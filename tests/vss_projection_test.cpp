#include "carla_ego_runtime/vss.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const carla_ego_runtime::VssDataPoint *Find(
    const carla_ego_runtime::VssSnapshot &snapshot, const std::string &path) {
  for (const auto &point : snapshot.data_points) {
    if (point.path == path) {
      return &point;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  using namespace carla_ego_runtime;

  NormalizedVehicleState state;
  state.run_id = "run-a";
  state.ego_vehicle_id = "9";
  state.frame_id = 100;
  state.simulation_time_s = 5.0;
  state.timestamp_utc = std::chrono::system_clock::time_point{} +
                        std::chrono::milliseconds(1234);
  state.speed_mps = 10.0;
  state.acceleration_iso_mps2 = {1.0, -2.0, 0.5};
  state.throttle_command = 0.125;
  state.brake_command = 0.995;
  state.gear = -1;
  state.engine_rpm = 1200.0;
  state.equivalent_front_axle_angle_iso_deg = 7.5;
  state.wheels[0] = {20.0, 23.76, -1.5, 0.2};
  state.wheels[1] = {21.0, 24.95, 1.5, 0.25};

  NormalizedGnssFix gnss;
  gnss.source_frame_id = 98;
  gnss.source_simulation_time_s = 4.9;
  gnss.timestamp_utc = std::chrono::system_clock::time_point{} +
                       std::chrono::milliseconds(1134);
  gnss.latitude_deg = 52.520008;
  gnss.longitude_deg = 13.404954;
  gnss.altitude_m = 37.25;

  const auto snapshot = ProjectToVss(state, gnss);
  Check(snapshot.timestamp == "1970-01-01T00:00:01.234Z",
        "UTC timestamp has millisecond precision and trailing Z");
  Check(std::get<double>(Find(snapshot, "Vehicle.Speed")->value) == 36.0,
        "m/s converted to km/h");
  Check(std::get<std::uint64_t>(
            Find(snapshot, "Vehicle.Chassis.Accelerator.PedalPosition")->value) ==
            13,
        "half-up accelerator percentage rounding");
  Check(std::get<std::uint64_t>(
            Find(snapshot, "Vehicle.Chassis.Brake.PedalPosition")->value) == 100,
        "half-up brake percentage rounding");
  Check(std::get<std::int64_t>(
            Find(snapshot, "Vehicle.Powertrain.Transmission.CurrentGear")->value) ==
            -1,
        "gear projected as signed integer");
  Check(Find(snapshot, "Vehicle.Chassis.Axle.Row1.SteeringAngle") != nullptr,
        "equivalent axle steering projected");
  Check(std::get<double>(
            Find(snapshot,
                 "Vehicle.Chassis.Axle.Row1.Wheel.Left.AngularSpeed")
                ->value) == 20.0,
        "standard wheel angular speed projected");
  Check(std::get<double>(
            Find(snapshot, "Vehicle.Chassis.Axle.Row1.Wheel.Left.Speed")
                ->value) == 23.76,
        "standard wheel linear speed projected");
  Check(std::get<double>(
            Find(snapshot,
                 "Vehicle.CarlaSimulation.ChaosWheel.Row1.Left."
                 "LateralSlipAngle")
                ->value) == -1.5,
        "simulator-specific lateral slip projected");
  Check(std::get<double>(
            Find(snapshot,
                 "Vehicle.CarlaSimulation.ChaosWheel.Row1.Right."
                 "LongitudinalSlip")
                ->value) == 0.25,
        "simulator-specific longitudinal slip projected");
  Check(std::get<double>(
            Find(snapshot, "Vehicle.CurrentLocation.Latitude")->value) ==
            52.520008,
        "GNSS latitude projected");
  Check(std::get<std::uint64_t>(
            Find(snapshot, "Vehicle.CarlaSimulation.GnssFrameId")->value) ==
            98,
        "GNSS source frame projected");
  Check(Find(snapshot, "Vehicle.CurrentLocation.Latitude")->timestamp ==
            "1970-01-01T00:00:01.134Z",
        "GNSS point retains sensor timestamp");
  Check(Find(snapshot, "Vehicle.Speed")->timestamp == snapshot.timestamp,
        "vehicle point retains state frame timestamp");

  LatestVssSignalStore store;
  Check(store.Publish(snapshot), "first frame accepted");
  Check(!store.Publish(snapshot), "duplicate frame rejected");
  auto newer = snapshot;
  newer.frame_id = 101;
  Check(store.Publish(newer), "newer frame accepted");
  Check(store.publish_count() == 2, "exactly one update counted per frame");
  Check(store.Latest()->frame_id == 101, "only latest snapshot retained");

  state.engine_rpm.reset();
  state.equivalent_front_axle_angle_iso_deg.reset();
  const auto missing = ProjectToVss(state);
  Check(Find(missing, "Vehicle.Powertrain.CombustionEngine.Speed") == nullptr,
        "unavailable RPM omitted");
  Check(Find(missing, "Vehicle.Chassis.Axle.Row1.SteeringAngle") == nullptr,
        "unavailable steering omitted");
  Check(Find(missing, "Vehicle.CurrentLocation.Latitude") == nullptr,
        "missing GNSS omitted instead of replaced by zero");

  if (failures == 0) {
    std::cout << "VSS projection tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
