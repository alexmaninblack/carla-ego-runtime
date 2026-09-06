#include "carla_ego_runtime/vss.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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

const carla_ego_runtime::VssDataPoint *Find(
    const carla_ego_runtime::VssSnapshot &snapshot, const std::string &path) {
  for (const auto &point : snapshot.data_points) {
    if (point.path == path) {
      return &point;
    }
  }
  return nullptr;
}

std::size_t CountControlResetFacts(
    const carla_ego_runtime::VssSnapshot &snapshot) {
  std::size_t count = 0;
  for (const auto &point : snapshot.data_points) {
    if (point.path == "Vehicle.CarlaSimulation.Control.ActiveMode" ||
        point.path == "Vehicle.CarlaSimulation.Control.TransitionState" ||
        point.path == "Vehicle.CarlaSimulation.Control.Generation" ||
        point.path == "Vehicle.CarlaSimulation.Reset.Generation" ||
        point.path == "Vehicle.CarlaSimulation.Reset.InProgress" ||
        point.path == "Vehicle.CarlaSimulation.Reset.Discontinuity") {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main() {
  using namespace carla_ego_runtime;

  static_assert(!std::is_default_constructible_v<SimulatorControlFacts>,
                "control facts must not fabricate a default safe state");

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

  const SimulatorControlFacts control_facts{
      state.frame_id, SimulatorDriveMode::kSafeStop,
      SimulatorTransitionState::kStable, 7, 3, false, true};

  const auto snapshot = ProjectToVss(state, gnss, control_facts);
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
  CheckNear(std::get<double>(
                Find(snapshot,
                     "Vehicle.Chassis.Axle.Row1.Wheel.Left.AngularSpeed")
                    ->value),
            1145.9155902616465, 1.0e-12,
            "standard wheel angular speed converted from rad/s to deg/s");
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
  Check(CountControlResetFacts(snapshot) == 6,
        "complete control/reset fact group projected");
  Check(std::get<std::string>(
            Find(snapshot, "Vehicle.CarlaSimulation.Control.ActiveMode")
                ->value) == "SAFE_STOP",
        "closed drive mode projected in accepted form");
  Check(std::get<std::string>(
            Find(snapshot,
                 "Vehicle.CarlaSimulation.Control.TransitionState")
                ->value) == "STABLE",
        "closed transition state projected in accepted form");
  Check(std::get<std::uint64_t>(
            Find(snapshot, "Vehicle.CarlaSimulation.Control.Generation")
                ->value) == 7,
        "control generation projected");
  Check(std::get<std::uint64_t>(
            Find(snapshot, "Vehicle.CarlaSimulation.Reset.Generation")
                ->value) == 3,
        "reset generation projected");
  Check(!std::get<bool>(
            Find(snapshot, "Vehicle.CarlaSimulation.Reset.InProgress")
                ->value),
        "reset in-progress boolean projected");
  Check(std::get<bool>(
            Find(snapshot, "Vehicle.CarlaSimulation.Reset.Discontinuity")
                ->value),
        "reset discontinuity boolean projected");
  for (const auto &path : {
           "Vehicle.CarlaSimulation.Control.ActiveMode",
           "Vehicle.CarlaSimulation.Control.TransitionState",
           "Vehicle.CarlaSimulation.Control.Generation",
           "Vehicle.CarlaSimulation.Reset.Generation",
           "Vehicle.CarlaSimulation.Reset.InProgress",
           "Vehicle.CarlaSimulation.Reset.Discontinuity"}) {
    Check(Find(snapshot, path)->timestamp == snapshot.timestamp,
          std::string(path) + " retains the physical frame timestamp");
  }

  const std::array<std::pair<SimulatorDriveMode, std::string>, 4> mode_cases{{
      {SimulatorDriveMode::kSafeStop, "SAFE_STOP"},
      {SimulatorDriveMode::kScenario, "SCENARIO"},
      {SimulatorDriveMode::kManual, "MANUAL"},
      {SimulatorDriveMode::kAutopilot, "AUTOPILOT"},
  }};
  for (const auto &[mode, expected] : mode_cases) {
    auto facts = control_facts;
    facts.active_mode = mode;
    const auto mapped = ProjectToVss(state, std::nullopt, facts);
    const auto *point =
        Find(mapped, "Vehicle.CarlaSimulation.Control.ActiveMode");
    Check(CountControlResetFacts(mapped) == 6 && point != nullptr,
          expected + " drive mode emits one complete fact group");
    if (point != nullptr) {
      Check(std::get<std::string>(point->value) == expected,
            expected + " drive mode mapping is exact");
    }
  }

  const std::array<std::pair<SimulatorTransitionState, std::string>, 3>
      transition_cases{{
          {SimulatorTransitionState::kStable, "STABLE"},
          {SimulatorTransitionState::kPreparing, "PREPARING"},
          {SimulatorTransitionState::kFailed, "FAILED"},
      }};
  for (const auto &[transition, expected] : transition_cases) {
    auto facts = control_facts;
    facts.transition_state = transition;
    const auto mapped = ProjectToVss(state, std::nullopt, facts);
    const auto *point =
        Find(mapped, "Vehicle.CarlaSimulation.Control.TransitionState");
    Check(CountControlResetFacts(mapped) == 6 && point != nullptr,
          expected + " transition emits one complete fact group");
    if (point != nullptr) {
      Check(std::get<std::string>(point->value) == expected,
            expected + " transition mapping is exact");
    }
  }

  LatestVssSignalStore store;
  Check(store.Publish(snapshot), "first frame accepted");
  Check(!store.Publish(snapshot), "duplicate frame rejected");
  Check(store.Latest()->timestamp == snapshot.timestamp &&
            Find(*store.Latest(), "Vehicle.Speed")->timestamp == snapshot.timestamp,
        "repeated cached reads retain acquisition time, not request time");
  auto newer = snapshot;
  newer.frame_id = 101;
  Check(store.Publish(newer), "newer frame accepted");
  Check(store.publish_count() == 2, "exactly one update counted per frame");
  Check(store.Latest()->frame_id == 101, "only latest snapshot retained");

  auto resumed = state;
  resumed.frame_id = 102;
  resumed.simulation_time_s += 0.05;
  resumed.timestamp_utc += std::chrono::seconds(3);
  const SimulatorControlFacts resumed_facts{
      resumed.frame_id, SimulatorDriveMode::kSafeStop,
      SimulatorTransitionState::kStable, 7, 3, false, false};
  const auto resumed_snapshot = ProjectToVss(resumed, gnss, resumed_facts);
  Check(store.Publish(resumed_snapshot), "new frame after real-time pause accepted");
  Check(resumed_snapshot.timestamp == "1970-01-01T00:00:04.234Z" &&
            Find(resumed_snapshot, "Vehicle.CarlaSimulation.Control.ActiveMode")
                    ->timestamp == resumed_snapshot.timestamp,
        "post-pause physics and Safe Stop facts share real acquisition UTC");
  Check(Find(resumed_snapshot, "Vehicle.CurrentLocation.Latitude")->timestamp ==
            "1970-01-01T00:00:01.134Z",
        "retained GNSS cannot be made fresh by new frame acquisition");

  state.engine_rpm.reset();
  state.equivalent_front_axle_angle_iso_deg.reset();
  const auto missing = ProjectToVss(state);
  Check(Find(missing, "Vehicle.Powertrain.CombustionEngine.Speed") == nullptr,
        "unavailable RPM omitted");
  Check(Find(missing, "Vehicle.Chassis.Axle.Row1.SteeringAngle") == nullptr,
        "unavailable steering omitted");
  Check(Find(missing, "Vehicle.CurrentLocation.Latitude") == nullptr,
        "missing GNSS omitted instead of replaced by zero");
  Check(CountControlResetFacts(missing) == 0,
        "missing control facts omit the complete group");
  Check(Find(missing, "Vehicle.CarlaSimulation.FrameId") != nullptr &&
            Find(missing, "Vehicle.Speed") != nullptr &&
            Find(missing,
                 "Vehicle.Chassis.Accelerator.PedalPosition") != nullptr &&
            Find(missing, "Vehicle.Chassis.Brake.PedalPosition") != nullptr,
        "missing control facts preserve the four physical Safe Stop facts");

  auto mismatched = control_facts;
  mismatched.source_frame_id = state.frame_id - 1;
  const auto wrong_frame = ProjectToVss(state, std::nullopt, mismatched);
  Check(CountControlResetFacts(wrong_frame) == 0,
        "wrong-frame control facts omit the complete group");
  Check(Find(wrong_frame, "Vehicle.CarlaSimulation.FrameId") != nullptr &&
            Find(wrong_frame, "Vehicle.Speed") != nullptr,
        "wrong-frame control facts do not suppress truthful vehicle state");

  auto invalid_mode = control_facts;
  invalid_mode.active_mode = static_cast<SimulatorDriveMode>(255);
  const auto invalid_context =
      ProjectToVss(state, std::nullopt, invalid_mode);
  Check(CountControlResetFacts(invalid_context) == 0,
        "invalid closed enum omits the complete group without a default");

  auto invalid_transition = control_facts;
  invalid_transition.transition_state =
      static_cast<SimulatorTransitionState>(255);
  const auto invalid_transition_context =
      ProjectToVss(state, std::nullopt, invalid_transition);
  Check(CountControlResetFacts(invalid_transition_context) == 0,
        "invalid transition omits the complete group without a default");

  if (failures == 0) {
    std::cout << "VSS projection tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
