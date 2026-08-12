#pragma once

namespace carla_ego_runtime {

struct CameraPose {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double pitch = 0.0;
  double yaw = 0.0;
  double roll = 0.0;
};

CameraPose InterpolateCameraPose(const CameraPose &previous,
                                 const CameraPose &latest, double factor);

} // namespace carla_ego_runtime
