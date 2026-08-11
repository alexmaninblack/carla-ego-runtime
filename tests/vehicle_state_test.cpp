#include "carla_ego_runtime/vehicle_state.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void CheckNear(double actual, double expected, double tolerance,
               const std::string &message) {
  Check(std::abs(actual - expected) <= tolerance, message);
}

template <typename Callable>
void CheckThrows(Callable callable, const std::string &message) {
  try {
    callable();
    Check(false, message);
  } catch (const std::exception &) {
  }
}

carla_ego_runtime::CarlaVehicleSample ValidSample() {
  carla_ego_runtime::CarlaVehicleSample sample;
  sample.run_id = "run-1";
  sample.ego_vehicle_id = "17";
  sample.frame_id = 42;
  sample.simulation_time_s = 2.1;
  sample.timestamp_utc = std::chrono::system_clock::time_point{};
  sample.velocity_world_mps = {3.0, 4.0, 0.0};
  sample.acceleration_vehicle_carla_mps2 = {1.0, 2.0, 3.0};
  sample.throttle_command = 0.125;
  sample.brake_command = 0.0;
  sample.steering_command = -0.25;
  sample.gear = -1;
  sample.engine_rpm = 1500.0;
  sample.front_left_wheel_angle_carla_deg = -12.0;
  sample.front_right_wheel_angle_carla_deg = -10.0;
  return sample;
}

}  // namespace

int main() {
  using namespace carla_ego_runtime;

  const auto state = NormalizeVehicleSample(ValidSample());
  CheckNear(state.speed_mps, 5.0, 1.0e-12, "velocity magnitude");
  CheckNear(state.acceleration_iso_mps2.x, 1.0, 1.0e-12,
            "longitudinal acceleration unchanged");
  CheckNear(state.acceleration_iso_mps2.y, -2.0, 1.0e-12,
            "CARLA Y-right converted to ISO Y-left");
  CheckNear(state.acceleration_iso_mps2.z, 3.0, 1.0e-12,
            "vertical acceleration unchanged");
  Check(state.front_left_wheel_angle_iso_deg == 12.0,
        "left wheel angle sign converted");
  Check(state.front_right_wheel_angle_iso_deg == 10.0,
        "right wheel angle sign converted");
  Check(state.equivalent_front_axle_angle_iso_deg.has_value(),
        "equivalent steering angle available");
  Check(*state.equivalent_front_axle_angle_iso_deg > 10.0 &&
            *state.equivalent_front_axle_angle_iso_deg < 12.0,
        "equivalent steering lies between road-wheel angles");

  const auto equal_angle = EquivalentFrontAxleAngleDegrees(-15.0, -15.0);
  Check(equal_angle.has_value(), "equal wheel angles accepted");
  CheckNear(*equal_angle, -15.0, 1.0e-12,
            "equal wheel angles preserve the same equivalent angle");
  Check(EquivalentFrontAxleAngleDegrees(10.0, -10.0) == std::nullopt,
        "contradictory wheel angles unavailable");
  Check(EquivalentFrontAxleAngleDegrees(90.0, 10.0) == std::nullopt,
        "impossible wheel angle unavailable");

  auto invalid = ValidSample();
  invalid.throttle_command = 1.01;
  CheckThrows([&] { NormalizeVehicleSample(invalid); },
              "out-of-range throttle rejected");
  invalid = ValidSample();
  invalid.gear = 128;
  CheckThrows([&] { NormalizeVehicleSample(invalid); },
              "gear outside VSS int8 rejected");
  invalid = ValidSample();
  invalid.engine_rpm = -1.0;
  Check(!NormalizeVehicleSample(invalid).engine_rpm.has_value(),
        "invalid optional RPM omitted instead of replaced by zero");

  const auto anchor_time = std::chrono::system_clock::time_point{} +
                           std::chrono::seconds(100);
  const SimulationClockAnchor anchor(25.0, anchor_time);
  Check(anchor.TimestampFor(25.5) == anchor_time + std::chrono::milliseconds(500),
        "simulation-time delta drives UTC timestamp");

  if (failures == 0) {
    std::cout << "vehicle state tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
