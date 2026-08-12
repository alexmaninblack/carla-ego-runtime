#include "carla_ego_runtime/chase_camera.hpp"

#include <cmath>
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

bool Near(double actual, double expected) {
  return std::abs(actual - expected) < 1e-9;
}

} // namespace

int main() {
  using carla_ego_runtime::CameraPose;
  using carla_ego_runtime::InterpolateCameraPose;

  const CameraPose previous{0.0, 2.0, 4.0, 0.0, 179.0, -10.0};
  const CameraPose latest{10.0, 4.0, 8.0, 20.0, -179.0, 10.0};
  const auto midpoint = InterpolateCameraPose(previous, latest, 0.5);
  Check(Near(midpoint.x, 5.0) && Near(midpoint.y, 3.0) &&
            Near(midpoint.z, 6.0),
        "position interpolation is linear");
  Check(Near(std::abs(midpoint.yaw), 180.0),
        "angle interpolation follows the shortest path across wraparound");

  const auto before = InterpolateCameraPose(previous, latest, -1.0);
  Check(Near(before.x, previous.x) && Near(before.yaw, previous.yaw),
        "negative interpolation factors clamp to the previous pose");
  const auto after = InterpolateCameraPose(previous, latest, 2.0);
  Check(Near(after.x, latest.x) &&
            Near(std::remainder(after.yaw - latest.yaw, 360.0), 0.0),
        "interpolation factors above one clamp to the latest pose");

  if (failures == 0) {
    std::cout << "chase camera tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
