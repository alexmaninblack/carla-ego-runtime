#include "carla_ego_runtime/runtime_options.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Callable>
void CheckThrows(Callable callable, const std::string &message) {
  try {
    callable();
    Check(false, message);
  } catch (const std::exception &) {
  }
}

}  // namespace

int main() {
  using carla_ego_runtime::Command;
  using carla_ego_runtime::ParseCommandLine;

  const auto defaults = ParseCommandLine({});
  Check(defaults.command == Command::kRun, "default command is run");
  Check(defaults.options.host == "127.0.0.1", "default host");
  Check(defaults.options.port == 2000, "default port");
  Check(defaults.options.role_name == "hero", "default role");
  Check(defaults.options.max_frames == 1, "one telemetry frame by default");
  Check(defaults.options.fixed_delta_seconds == 0.05, "default fixed delta");
  Check(defaults.options.log_every_frames == 1, "log every frame by default");
  Check(defaults.options.tick_owner, "tick ownership enabled by default");
  Check(defaults.options.spawn_if_missing, "spawning enabled by default");
  Check(defaults.options.require_matching_versions,
        "version match required by default");
  Check(!defaults.options.real_time, "real-time pacing disabled by default");
  Check(!defaults.options.autopilot, "autopilot disabled by default");
  Check(!defaults.options.chase_camera, "chase camera disabled by default");

  const auto custom = ParseCommandLine(
      {"--host", "carla.local", "--port", "2100", "--timeout-ms", "5000",
       "--role-name", "ego", "--blueprint", "vehicle.tesla.model3",
       "--spawn-point-index", "7", "--run-seconds", "15", "--max-frames",
       "42", "--fixed-delta-seconds", "0.1", "--log-every-frames", "10",
       "--observe-ticks", "--real-time", "--autopilot", "--chase-camera",
       "--no-spawn", "--allow-version-mismatch"});
  Check(custom.options.host == "carla.local", "custom host");
  Check(custom.options.port == 2100, "custom port");
  Check(custom.options.timeout_ms == 5000, "custom timeout");
  Check(custom.options.role_name == "ego", "custom role");
  Check(custom.options.blueprint_id == "vehicle.tesla.model3",
        "custom blueprint");
  Check(custom.options.spawn_point_index == 7, "custom spawn point");
  Check(custom.options.run_seconds == 15, "custom run duration");
  Check(custom.options.max_frames == 42, "custom frame limit");
  Check(custom.options.fixed_delta_seconds == 0.1, "custom fixed delta");
  Check(custom.options.log_every_frames == 10, "custom log interval");
  Check(!custom.options.tick_owner, "observer mode");
  Check(!custom.options.spawn_if_missing, "spawning disabled");
  Check(!custom.options.require_matching_versions, "version mismatch allowed");
  Check(custom.options.real_time, "real-time pacing enabled");
  Check(custom.options.autopilot, "autopilot enabled");
  Check(custom.options.chase_camera, "chase camera enabled");

  Check(ParseCommandLine({"--help"}).command == Command::kHelp, "help command");
  Check(ParseCommandLine({"--version"}).command == Command::kVersion,
        "version command");
  Check(ParseCommandLine({"--run-seconds", "2"}).options.max_frames == 0,
        "wall-clock run overrides implicit one-frame limit");

  CheckThrows([] { ParseCommandLine({"--port", "0"}); }, "zero port rejected");
  CheckThrows([] { ParseCommandLine({"--port", "65536"}); },
              "oversized port rejected");
  CheckThrows([] { ParseCommandLine({"--timeout-ms"}); },
              "missing value rejected");
  CheckThrows([] { ParseCommandLine({"--host", "--no-spawn"}); },
              "option token rejected as a value");
  CheckThrows([] { ParseCommandLine({"--unknown"}); },
              "unknown option rejected");
  CheckThrows([] { ParseCommandLine({"--fixed-delta-seconds", "0"}); },
              "zero fixed delta rejected");
  CheckThrows([] { ParseCommandLine({"--fixed-delta-seconds", "1.1"}); },
              "oversized fixed delta rejected");
  CheckThrows([] { ParseCommandLine({"--log-every-frames", "0"}); },
              "zero log interval rejected");

  if (failures == 0) {
    std::cout << "runtime option tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
