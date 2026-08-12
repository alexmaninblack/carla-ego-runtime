#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carla_ego_runtime {

struct RuntimeOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 2000;
  std::uint32_t timeout_ms = 10000;
  std::string role_name = "hero";
  std::string blueprint_id = "vehicle.lincoln.mkz";
  std::size_t spawn_point_index = 0;
  std::uint32_t run_seconds = 0;
  std::uint64_t max_frames = 1;
  double fixed_delta_seconds = 0.05;
  double gnss_sensor_tick_seconds = 0.1;
  double gnss_max_age_seconds = 0.25;
  std::uint64_t log_every_frames = 1;
  bool tick_owner = true;
  bool spawn_if_missing = true;
  bool require_matching_versions = true;
  bool real_time = false;
  bool autopilot = false;
  bool chase_camera = false;
};

enum class Command {
  kRun,
  kHelp,
  kVersion,
};

struct ParsedCommandLine {
  Command command = Command::kRun;
  RuntimeOptions options;
};

ParsedCommandLine ParseCommandLine(const std::vector<std::string> &arguments);
std::string Usage();

}  // namespace carla_ego_runtime
