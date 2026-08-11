#include "carla_ego_runtime/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "carla-ego-runtime " << carla_ego_runtime::kVersion << '\n';
    return 0;
  }

  std::cout
      << "carla-ego-runtime scaffold " << carla_ego_runtime::kVersion << '\n'
      << "CARLA connectivity and telemetry are not implemented yet.\n";
  return 0;
}
