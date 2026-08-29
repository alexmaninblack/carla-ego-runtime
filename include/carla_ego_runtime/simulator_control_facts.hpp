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
  constexpr SimulatorControlFacts(
      std::uint64_t attributable_source_frame_id,
      SimulatorDriveMode applied_mode,
      SimulatorTransitionState applied_transition_state,
      std::uint64_t accepted_control_generation,
      std::uint64_t successful_reset_generation, bool is_reset_in_progress,
      bool is_reset_discontinuity)
      : source_frame_id(attributable_source_frame_id),
        active_mode(applied_mode),
        transition_state(applied_transition_state),
        control_generation(accepted_control_generation),
        reset_generation(successful_reset_generation),
        reset_in_progress(is_reset_in_progress),
        reset_discontinuity(is_reset_discontinuity) {}

  std::uint64_t source_frame_id;
  SimulatorDriveMode active_mode;
  SimulatorTransitionState transition_state;
  std::uint64_t control_generation;
  std::uint64_t reset_generation;
  bool reset_in_progress;
  bool reset_discontinuity;
};

}  // namespace carla_ego_runtime
