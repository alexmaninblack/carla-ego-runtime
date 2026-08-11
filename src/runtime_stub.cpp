#include "carla_ego_runtime/runtime.hpp"

#include <iostream>

namespace carla_ego_runtime {

int RunRuntime(const RuntimeOptions &) {
  std::cerr
      << "This build does not include CARLA connectivity. Reconfigure with\n"
      << "  -DCARLA_EGO_WITH_CARLA=ON -DCMAKE_PREFIX_PATH=/path/to/carla-install\n";
  return 3;
}

}  // namespace carla_ego_runtime
