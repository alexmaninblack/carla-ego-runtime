#include "carla_ego_runtime/runtime.hpp"
#include "carla_ego_runtime/runtime_options.hpp"
#include "carla_ego_runtime/version.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  try {
    const auto command_line = carla_ego_runtime::ParseCommandLine(arguments);
    if (command_line.command == carla_ego_runtime::Command::kHelp) {
      std::cout << carla_ego_runtime::Usage();
      return 0;
    }
    if (command_line.command == carla_ego_runtime::Command::kVersion) {
      std::cout << "carla-ego-runtime " << carla_ego_runtime::kVersion << '\n';
      return 0;
    }
    return carla_ego_runtime::RunRuntime(command_line.options);
  } catch (const std::exception &error) {
    std::cerr << "Argument error: " << error.what() << "\n\n"
              << carla_ego_runtime::Usage();
    return 2;
  }
}
