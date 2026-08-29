#pragma once

#include <cstdint>

namespace carla_ego_runtime {

// Closed control values accepted by the D4-004 engineering projection. The
// type deliberately carries facts only; it contains no Safe Stop policy.
enum class SimulatorDriveMode {
  kSafeStop,
  kScenario,
  kManual,
  kAutopilot,
};

enum class SimulatorTransitionState {
  kStable,
  kPreparing,
  kFailed,
};

// A complete control/reset fact group attributable to one CARLA frame. The
// VSS projection emits the group only when source_frame_id matches the
// physical vehicle state frame, preventing a mixed-frame Safe Stop view.
struct SimulatorControlFacts {
  std::uint64_t source_frame_id = 0;
  SimulatorDriveMode active_mode = SimulatorDriveMode::kSafeStop;
  SimulatorTransitionState transition_state =
      SimulatorTransitionState::kStable;
  std::uint64_t control_generation = 0;
  std::uint64_t reset_generation = 0;
  bool reset_in_progress = false;
  bool reset_discontinuity = false;
};

}  // namespace carla_ego_runtime
