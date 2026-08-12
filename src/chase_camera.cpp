#include "carla_ego_runtime/chase_camera.hpp"

#include <algorithm>
#include <cmath>

namespace carla_ego_runtime {
namespace {

double InterpolateAngle(double previous, double latest, double factor) {
  const auto delta = std::remainder(latest - previous, 360.0);
  return previous + factor * delta;
}

} // namespace

CameraPose InterpolateCameraPose(const CameraPose &previous,
                                 const CameraPose &latest, double factor) {
  factor = std::clamp(factor, 0.0, 1.0);
  const auto interpolate = [factor](double from, double to) {
    return from + factor * (to - from);
  };
  return {
      interpolate(previous.x, latest.x),
      interpolate(previous.y, latest.y),
      interpolate(previous.z, latest.z),
      InterpolateAngle(previous.pitch, latest.pitch, factor),
      InterpolateAngle(previous.yaw, latest.yaw, factor),
      InterpolateAngle(previous.roll, latest.roll, factor),
  };
}

} // namespace carla_ego_runtime
