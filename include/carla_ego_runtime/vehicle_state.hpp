#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace carla_ego_runtime {

struct Vector3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

inline constexpr std::size_t kRoadWheelCount = 4;

// CARLA/Chaos reports wheels in FL, FR, RL, RR order. The radius is retained
// with each live solver sample so the normalization boundary can project both
// angular and linear wheel speed without relying on a vehicle-specific
// constant.
struct CarlaWheelSample {
  std::optional<double> angular_speed_rad_s;
  std::optional<double> radius_m;
  std::optional<double> lateral_slip_angle_deg;
  std::optional<double> longitudinal_slip;
};

struct NormalizedWheelState {
  std::optional<double> angular_speed_rad_s;
  std::optional<double> speed_kmh;
  std::optional<double> lateral_slip_angle_deg;
  std::optional<double> longitudinal_slip;
};

// Transport-independent input to the normalization boundary. Acceleration is
// already expressed in CARLA vehicle axes (X forward, Y right, Z up).
struct CarlaVehicleSample {
  std::string run_id;
  std::string ego_vehicle_id;
  std::uint64_t frame_id = 0;
  double simulation_time_s = 0.0;
  std::chrono::system_clock::time_point timestamp_utc;
  Vector3 velocity_world_mps;
  Vector3 acceleration_vehicle_carla_mps2;
  double throttle_command = 0.0;
  double brake_command = 0.0;
  double steering_command = 0.0;
  std::int32_t gear = 0;
  std::optional<double> engine_rpm;
  std::optional<double> front_left_wheel_angle_carla_deg;
  std::optional<double> front_right_wheel_angle_carla_deg;
  std::array<CarlaWheelSample, kRoadWheelCount> wheels;
};

struct NormalizedVehicleState {
  std::string run_id;
  std::string ego_vehicle_id;
  std::uint64_t frame_id = 0;
  double simulation_time_s = 0.0;
  std::chrono::system_clock::time_point timestamp_utc;
  double speed_mps = 0.0;
  Vector3 acceleration_iso_mps2;
  double throttle_command = 0.0;
  double brake_command = 0.0;
  double steering_command = 0.0;
  std::int8_t gear = 0;
  std::optional<double> engine_rpm;
  std::optional<double> front_left_wheel_angle_iso_deg;
  std::optional<double> front_right_wheel_angle_iso_deg;
  std::optional<double> equivalent_front_axle_angle_iso_deg;
  std::array<NormalizedWheelState, kRoadWheelCount> wheels;
};

// Returns the equivalent single-track road-wheel angle. Positive is left,
// negative is right. Invalid or contradictory wheel angles are unavailable.
std::optional<double> EquivalentFrontAxleAngleDegrees(
    double front_left_iso_deg, double front_right_iso_deg);

NormalizedVehicleState NormalizeVehicleSample(const CarlaVehicleSample &sample);

class SimulationClockAnchor {
 public:
  SimulationClockAnchor(double simulation_time_s,
                        std::chrono::system_clock::time_point timestamp_utc);

  std::chrono::system_clock::time_point TimestampFor(
      double simulation_time_s) const;

 private:
  double simulation_time_s_;
  std::chrono::system_clock::time_point timestamp_utc_;
};

}  // namespace carla_ego_runtime
