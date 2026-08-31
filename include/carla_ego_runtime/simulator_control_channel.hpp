#pragma once

#include "carla_ego_runtime/simulator_control_facts.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace carla_ego_runtime {

inline constexpr std::size_t kMaximumControlFrameBodyBytes = 4096;
inline constexpr std::size_t kControlFrameLengthBytes = 4;
inline constexpr std::size_t kMaximumUnmatchedSimulatorRecords = 4;
inline constexpr auto kMaximumSimulatorRecordResidence =
    std::chrono::milliseconds(250);

struct SimulatorControlRecord {
  std::string run_id;
  std::uint64_t ego_actor_id;
  std::uint64_t frame_id;
  double simulation_time_s;
  SimulatorDriveMode active_mode;
  SimulatorTransitionState transition_state;
  std::uint64_t control_generation;
  std::uint64_t reset_generation;
  bool reset_in_progress;
  bool reset_discontinuity;
};

struct SimulatorPhysicalFrame {
  std::string run_id;
  std::uint64_t ego_actor_id;
  std::uint64_t frame_id;
  double simulation_time_s;
};

struct SimulatorControlDecodeResult {
  std::optional<SimulatorControlRecord> record;
  std::string error;
};

SimulatorControlDecodeResult DecodeSimulatorControlRecord(
    std::string_view payload);

struct SimulatorControlDiagnostics {
  std::uint64_t connections_accepted = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t frames_accepted = 0;
  std::uint64_t records_matched = 0;
  std::uint64_t malformed_records = 0;
  std::uint64_t wrong_identity_records = 0;
  std::uint64_t duplicate_or_out_of_order_records = 0;
  std::uint64_t generation_regressions = 0;
  std::uint64_t peer_rejections = 0;
  std::uint64_t capacity_evictions = 0;
  std::uint64_t expired_records = 0;
};

// Dependency-free, deterministic join core used by the Unix receiver and its
// synthetic tests. A match is retained separately after both source records
// have been removed, so no source record can be reused for a later frame.
class SimulatorControlJoin {
 public:
  using Clock = std::chrono::steady_clock;

  SimulatorControlJoin(std::string expected_run_id,
                       std::uint64_t expected_ego_actor_id);

  void OfferPhysical(SimulatorPhysicalFrame frame, Clock::time_point received);
  void OfferControl(SimulatorControlRecord record, Clock::time_point received);
  std::optional<SimulatorControlFacts> TakeMatch(std::uint64_t frame_id,
                                                  double simulation_time_s);
  void Expire(Clock::time_point now);
  void NoteFrameReceived();
  void NoteConnectionAccepted();
  void NoteMalformedRecord();
  void NotePeerRejection();
  void InvalidateControl() noexcept;

  const SimulatorControlDiagnostics &diagnostics() const;

 private:
  struct PendingPhysical {
    SimulatorPhysicalFrame frame;
    Clock::time_point received;
  };
  struct PendingControl {
    SimulatorControlRecord record;
    Clock::time_point received;
  };
  struct Match {
    std::uint64_t frame_id;
    double simulation_time_s;
    SimulatorControlFacts facts;
  };

  void MatchAvailable();
  void EnforceCapacity();

  std::string expected_run_id_;
  std::uint64_t expected_ego_actor_id_;
  std::deque<PendingPhysical> physical_;
  std::deque<PendingControl> control_;
  std::deque<Match> matches_;
  std::optional<std::uint64_t> last_physical_frame_id_;
  std::optional<std::uint64_t> last_control_frame_id_;
  std::optional<std::uint64_t> last_control_generation_;
  std::optional<std::uint64_t> last_reset_generation_;
  SimulatorControlDiagnostics diagnostics_;
};

struct SimulatorControlChannelConfig {
  std::string socket_path;
  std::string expected_run_id;
  std::uint64_t expected_ego_actor_id;
};

// Cross-platform runtime transport. It listens on one private
// AF_UNIX/SOCK_STREAM endpoint, accepts one same-effective-UID producer and
// never turns transport loss into a controller or Safe Stop conclusion.
class SimulatorControlChannel {
 public:
  explicit SimulatorControlChannel(SimulatorControlChannelConfig config);
  SimulatorControlChannel(const SimulatorControlChannel &) = delete;
  SimulatorControlChannel &operator=(const SimulatorControlChannel &) = delete;
  ~SimulatorControlChannel();

  std::optional<SimulatorControlFacts> WaitFor(
      const SimulatorPhysicalFrame &frame,
      std::chrono::milliseconds maximum_wait =
          kMaximumSimulatorRecordResidence);
  SimulatorControlDiagnostics diagnostics() const;

 private:
  enum class State { kListening, kConnected, kUnavailable };

  bool AcceptOne();
  bool ReceiveOne();
  bool ConsumeFrames();
  void MakeUnavailable(bool malformed_partial = false) noexcept;
  void RemoveOwnedSocket() noexcept;

  SimulatorControlChannelConfig config_;
  SimulatorControlJoin join_;
  int listener_fd_ = -1;
  int connection_fd_ = -1;
  State state_ = State::kListening;
  std::array<char, kControlFrameLengthBytes + kMaximumControlFrameBodyBytes>
      receive_buffer_{};
  std::size_t receive_size_ = 0;
  std::uint64_t socket_device_ = 0;
  std::uint64_t socket_inode_ = 0;
};

}  // namespace carla_ego_runtime
