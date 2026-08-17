#include "carla_ego_runtime/vehicle_state.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace carla_ego_runtime {
namespace {

constexpr double kPi = 3.14159265358979323846;

void RequireFinite(double value, std::string_view name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void RequireUnitInterval(double value, std::string_view name) {
  RequireFinite(value, name);
  if (value < 0.0 || value > 1.0) {
    throw std::out_of_range(std::string(name) + " must be in [0, 1]");
  }
}

void RequireVectorFinite(const Vector3 &value, std::string_view name) {
  RequireFinite(value.x, name);
  RequireFinite(value.y, name);
  RequireFinite(value.z, name);
}

std::optional<double> NormalizeWheelAngle(
    const std::optional<double> &carla_angle_deg) {
  if (!carla_angle_deg.has_value() || !std::isfinite(*carla_angle_deg) ||
      std::abs(*carla_angle_deg) >= 90.0) {
    return std::nullopt;
  }
  // CARLA/Unreal has Y to the right; ISO 8855 steering is positive left.
  return -*carla_angle_deg;
}

std::optional<double> FiniteValue(const std::optional<double> &value) {
  return value.has_value() && std::isfinite(*value) ? value : std::nullopt;
}

NormalizedWheelState NormalizeWheelState(const CarlaWheelSample &sample) {
  NormalizedWheelState wheel;
  if (const auto angular_speed = FiniteValue(sample.angular_speed_rad_s);
      angular_speed.has_value()) {
    // Wheel orientation can make the solver sign vehicle-specific. VSS wheel
    // speed is a magnitude, so expose a stable non-negative angular speed.
    wheel.angular_speed_rad_s = std::abs(*angular_speed);
    if (const auto radius = FiniteValue(sample.radius_m);
        radius.has_value() && *radius > 0.0) {
      wheel.speed_kmh = std::abs(*angular_speed) * *radius * 3.6;
    }
  }
  wheel.lateral_slip_angle_deg =
      FiniteValue(sample.lateral_slip_angle_deg);
  wheel.longitudinal_slip = FiniteValue(sample.longitudinal_slip);
  return wheel;
}

}  // namespace

std::optional<double> EquivalentFrontAxleAngleDegrees(
    double front_left_iso_deg, double front_right_iso_deg) {
  if (!std::isfinite(front_left_iso_deg) ||
      !std::isfinite(front_right_iso_deg) ||
      std::abs(front_left_iso_deg) >= 90.0 ||
      std::abs(front_right_iso_deg) >= 90.0) {
    return std::nullopt;
  }

  constexpr double kEpsilon = 1.0e-12;
  if (std::abs(front_left_iso_deg) < kEpsilon &&
      std::abs(front_right_iso_deg) < kEpsilon) {
    return 0.0;
  }
  if (front_left_iso_deg * front_right_iso_deg < 0.0) {
    return std::nullopt;
  }

  const double left_tangent =
      std::tan(front_left_iso_deg * kPi / 180.0);
  const double right_tangent =
      std::tan(front_right_iso_deg * kPi / 180.0);
  const double denominator = left_tangent + right_tangent;
  if (std::abs(denominator) < kEpsilon) {
    return std::nullopt;
  }

  const double equivalent_tangent =
      2.0 * left_tangent * right_tangent / denominator;
  return std::atan(equivalent_tangent) * 180.0 / kPi;
}

NormalizedVehicleState NormalizeVehicleSample(const CarlaVehicleSample &sample) {
  if (sample.run_id.empty()) {
    throw std::invalid_argument("run_id must not be empty");
  }
  if (sample.ego_vehicle_id.empty()) {
    throw std::invalid_argument("ego_vehicle_id must not be empty");
  }
  RequireFinite(sample.simulation_time_s, "simulation_time_s");
  if (sample.simulation_time_s < 0.0) {
    throw std::out_of_range("simulation_time_s must not be negative");
  }
  RequireVectorFinite(sample.velocity_world_mps, "velocity_world_mps");
  RequireVectorFinite(sample.acceleration_vehicle_carla_mps2,
                      "acceleration_vehicle_carla_mps2");
  RequireUnitInterval(sample.throttle_command, "throttle_command");
  RequireUnitInterval(sample.brake_command, "brake_command");
  RequireFinite(sample.steering_command, "steering_command");
  if (sample.steering_command < -1.0 || sample.steering_command > 1.0) {
    throw std::out_of_range("steering_command must be in [-1, 1]");
  }
  if (sample.gear < std::numeric_limits<std::int8_t>::min() ||
      sample.gear > std::numeric_limits<std::int8_t>::max()) {
    throw std::out_of_range("gear does not fit VSS int8");
  }

  NormalizedVehicleState state;
  state.run_id = sample.run_id;
  state.ego_vehicle_id = sample.ego_vehicle_id;
  state.frame_id = sample.frame_id;
  state.simulation_time_s = sample.simulation_time_s;
  state.timestamp_utc = sample.timestamp_utc;
  state.speed_mps = std::sqrt(sample.velocity_world_mps.x *
                                  sample.velocity_world_mps.x +
                              sample.velocity_world_mps.y *
                                  sample.velocity_world_mps.y +
                              sample.velocity_world_mps.z *
                                  sample.velocity_world_mps.z);
  state.acceleration_iso_mps2 = {
      sample.acceleration_vehicle_carla_mps2.x,
      -sample.acceleration_vehicle_carla_mps2.y,
      sample.acceleration_vehicle_carla_mps2.z};
  state.throttle_command = sample.throttle_command;
  state.brake_command = sample.brake_command;
  state.steering_command = sample.steering_command;
  state.gear = static_cast<std::int8_t>(sample.gear);

  if (sample.engine_rpm.has_value() && std::isfinite(*sample.engine_rpm) &&
      *sample.engine_rpm >= 0.0) {
    state.engine_rpm = sample.engine_rpm;
  }
  state.front_left_wheel_angle_iso_deg =
      NormalizeWheelAngle(sample.front_left_wheel_angle_carla_deg);
  state.front_right_wheel_angle_iso_deg =
      NormalizeWheelAngle(sample.front_right_wheel_angle_carla_deg);
  if (state.front_left_wheel_angle_iso_deg.has_value() &&
      state.front_right_wheel_angle_iso_deg.has_value()) {
    state.equivalent_front_axle_angle_iso_deg =
        EquivalentFrontAxleAngleDegrees(
            *state.front_left_wheel_angle_iso_deg,
            *state.front_right_wheel_angle_iso_deg);
  }
  for (std::size_t index = 0; index < state.wheels.size(); ++index) {
    state.wheels[index] = NormalizeWheelState(sample.wheels[index]);
  }
  return state;
}

SimulationClockAnchor::SimulationClockAnchor(
    double simulation_time_s,
    std::chrono::system_clock::time_point timestamp_utc)
    : simulation_time_s_(simulation_time_s), timestamp_utc_(timestamp_utc) {
  RequireFinite(simulation_time_s_, "simulation_time_s");
}

std::chrono::system_clock::time_point SimulationClockAnchor::TimestampFor(
    double simulation_time_s) const {
  RequireFinite(simulation_time_s, "simulation_time_s");
  const auto elapsed = std::chrono::duration<double>(simulation_time_s -
                                                      simulation_time_s_);
  return timestamp_utc_ +
         std::chrono::duration_cast<std::chrono::system_clock::duration>(
             elapsed);
}

}  // namespace carla_ego_runtime
