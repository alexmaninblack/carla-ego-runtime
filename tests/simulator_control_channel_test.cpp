#include "carla_ego_runtime/simulator_control_channel.hpp"
#include "carla_ego_runtime/runtime_options.hpp"
#include "carla_ego_runtime/vss.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string Payload(std::uint64_t frame = 5201,
                    double simulation_time = 260.05,
                    std::uint64_t control_generation = 13,
                    std::uint64_t reset_generation = 5,
                    bool reset_discontinuity = true) {
  return "{\"schemaVersion\":1,\"runId\":\"run-a\",\"egoActorId\":42,"
         "\"frameId\":" + std::to_string(frame) +
         ",\"simulationTime\":" + std::to_string(simulation_time) +
         ",\"activeMode\":\"SCENARIO\",\"transitionState\":\"STABLE\","
         "\"controlGeneration\":" + std::to_string(control_generation) +
         ",\"resetGeneration\":" + std::to_string(reset_generation) +
         ",\"resetInProgress\":false,\"resetDiscontinuity\":" +
         (reset_discontinuity ? "true" : "false") + "}";
}

carla_ego_runtime::SimulatorControlRecord Record(
    std::uint64_t frame, double simulation_time,
    std::uint64_t control_generation = 1,
    std::uint64_t reset_generation = 0,
    bool reset_discontinuity = false) {
  return {"run-a",
          42,
          frame,
          simulation_time,
          carla_ego_runtime::SimulatorDriveMode::kManual,
          carla_ego_runtime::SimulatorTransitionState::kStable,
          control_generation,
          reset_generation,
          false,
          reset_discontinuity};
}

const carla_ego_runtime::VssDataPoint *Find(
    const carla_ego_runtime::VssSnapshot &snapshot, const std::string &path) {
  for (const auto &point : snapshot.data_points) {
    if (point.path == path) {
      return &point;
    }
  }
  return nullptr;
}

void CheckDecoder() {
  using namespace carla_ego_runtime;
  const auto valid = DecodeSimulatorControlRecord(Payload());
  Check(valid.record.has_value(), "canonical controller record is accepted");
  if (valid.record.has_value()) {
    Check(valid.record->run_id == "run-a", "run identity is decoded");
    Check(valid.record->ego_actor_id == 42, "ego actor identity is decoded");
    Check(valid.record->frame_id == 5201, "frame identity is decoded");
    Check(std::abs(valid.record->simulation_time_s - 260.05) < 1.0e-12,
          "simulation time is decoded");
    Check(valid.record->active_mode == SimulatorDriveMode::kScenario,
          "closed drive mode is decoded");
    Check(valid.record->reset_discontinuity,
          "reset discontinuity is decoded");
  }

  for (const auto &invalid : {
           std::string{},
           std::string{"[]"},
           std::string{"{\"schemaVersion\":2}"},
           std::string{"{\"schemaVersion\":1,\"unknown\":0}"},
           std::string{"{\"schemaVersion\":1,\"schemaVersion\":1}"},
           std::string{"{\"schemaVersion\":true}"},
           std::string{"{\"schemaVersion\":1"},
       }) {
    Check(!DecodeSimulatorControlRecord(invalid).record.has_value(),
          "malformed/unknown/incomplete record is rejected");
  }

  auto wrong_mode = Payload();
  const auto mode = wrong_mode.find("SCENARIO");
  wrong_mode.replace(mode, std::string("SCENARIO").size(), "DRIVINGX");
  Check(!DecodeSimulatorControlRecord(wrong_mode).record.has_value(),
        "unknown mode is rejected");

  auto wrong_type = Payload();
  const auto actor = wrong_type.find("42", wrong_type.find("egoActorId"));
  wrong_type.replace(actor, 2, "true");
  Check(!DecodeSimulatorControlRecord(wrong_type).record.has_value(),
        "boolean is not accepted as uint64");

  auto conflicting_reset = Payload();
  const auto in_progress = conflicting_reset.find("false");
  conflicting_reset.replace(in_progress, 5, "true");
  Check(!DecodeSimulatorControlRecord(conflicting_reset).record.has_value(),
        "in-progress discontinuity conflict is rejected");

  for (const auto &invalid_number : {
           std::string{"\"simulationTime\":-1.000000"},
           std::string{"\"frameId\":-1"},
           std::string{"\"controlGeneration\":18446744073709551616"},
       }) {
    auto invalid = Payload();
    const auto key_end = invalid_number.find(':');
    const auto key = invalid_number.substr(0, key_end);
    const auto begin = invalid.find(key);
    const auto value_begin = begin + key.size() + 1;
    const auto value_end = invalid.find(',', value_begin);
    invalid.replace(value_begin, value_end - value_begin,
                    invalid_number.substr(key_end + 1));
    Check(!DecodeSimulatorControlRecord(invalid).record.has_value(),
          "negative or uint64-overflow number is rejected");
  }

  std::string invalid_utf8 = Payload();
  invalid_utf8.insert(invalid_utf8.find("run-a") + 1, 1,
                      static_cast<char>(0xff));
  Check(!DecodeSimulatorControlRecord(invalid_utf8).record.has_value(),
        "invalid UTF-8 is rejected before JSON use");

  Check(!DecodeSimulatorControlRecord(
             std::string(kMaximumControlDatagramBytes + 1, ' '))
             .record.has_value(),
        "oversize datagram is rejected");
}

void CheckRuntimeOptions() {
  using namespace carla_ego_runtime;
  const auto parsed = ParseCommandLine(
      {"--observe-ticks", "--control-facts-socket", "/run/demo/facts.sock",
       "--simulator-run-id", "run-a"});
  Check(parsed.options.control_facts_socket_file == "/run/demo/facts.sock",
        "explicit controller facts socket is retained");
  Check(parsed.options.simulator_run_id == "run-a",
        "explicit simulator run identity is retained");
  for (const auto &arguments : {
           std::vector<std::string>{"--observe-ticks", "--control-facts-socket",
                                    "/run/demo/facts.sock"},
           std::vector<std::string>{"--observe-ticks", "--simulator-run-id",
                                    "run-a"},
           std::vector<std::string>{"--control-facts-socket",
                                    "/run/demo/facts.sock",
                                    "--simulator-run-id", "run-a"},
       }) {
    bool rejected = false;
    try {
      (void)ParseCommandLine(arguments);
    } catch (const std::exception &) {
      rejected = true;
    }
    Check(rejected, "partial or tick-owning handoff options are rejected");
  }
}

void CheckJoin() {
  using namespace carla_ego_runtime;
  using Clock = SimulatorControlJoin::Clock;
  const auto started = Clock::time_point{};

  SimulatorControlJoin control_first("run-a", 42);
  control_first.NoteDatagramReceived();
  control_first.OfferControl(Record(1, 0.05), started);
  control_first.OfferPhysical({"run-a", 42, 1, 0.05},
                              started + std::chrono::milliseconds(10));
  const auto first = control_first.TakeMatch(1, 0.05);
  Check(first.has_value(), "control-first arrival joins exact frame/time");
  Check(!control_first.TakeMatch(1, 0.05).has_value(),
        "a joined source record cannot be reused");

  SimulatorControlJoin physical_first("run-a", 42);
  physical_first.OfferPhysical({"run-a", 42, 2, 0.10}, started);
  physical_first.NoteDatagramReceived();
  physical_first.OfferControl(Record(2, 0.10),
                              started + std::chrono::milliseconds(10));
  Check(physical_first.TakeMatch(2, 0.10).has_value(),
        "physical-first arrival joins exact frame/time");

  SimulatorControlJoin wrong_time("run-a", 42);
  wrong_time.OfferPhysical({"run-a", 42, 3, 0.15}, started);
  wrong_time.OfferControl(Record(3, 0.1500001), started);
  Check(!wrong_time.TakeMatch(3, 0.15).has_value(),
        "same frame with different simulation time never joins");
  wrong_time.Expire(started + std::chrono::milliseconds(250));
  Check(wrong_time.diagnostics().expired_records == 2,
        "both unmatched sides expire at the fixed residence bound");

  SimulatorControlJoin identity("run-a", 42);
  auto wrong_run = Record(4, 0.20);
  wrong_run.run_id = "run-b";
  identity.OfferControl(wrong_run, started);
  auto wrong_ego = Record(5, 0.25);
  wrong_ego.ego_actor_id = 43;
  identity.OfferControl(wrong_ego, started);
  Check(identity.diagnostics().wrong_identity_records == 2,
        "wrong run and ego records are discarded");

  SimulatorControlJoin ordered("run-a", 42);
  ordered.OfferControl(Record(10, 0.5, 5), started);
  ordered.OfferControl(Record(10, 0.5, 5), started);
  ordered.OfferControl(Record(9, 0.45, 5), started);
  ordered.OfferControl(Record(11, 0.55, 4), started);
  Check(ordered.diagnostics().duplicate_or_out_of_order_records == 2,
        "duplicate and out-of-order control frames are discarded");
  Check(ordered.diagnostics().generation_regressions == 1,
        "control-generation regression is discarded");

  SimulatorControlJoin discontinuity("run-a", 42);
  discontinuity.OfferControl(Record(20, 1.0, 6, 2, true), started);
  discontinuity.OfferControl(Record(21, 1.05, 6, 2, true), started);
  Check(discontinuity.diagnostics().generation_regressions == 1,
        "reset discontinuity cannot repeat for one generation");

  SimulatorControlJoin bounded("run-a", 42);
  for (std::uint64_t frame = 30; frame < 35; ++frame) {
    bounded.OfferControl(Record(frame, static_cast<double>(frame), frame),
                         started + std::chrono::milliseconds(frame));
  }
  Check(bounded.diagnostics().capacity_evictions == 1,
        "fifth unmatched control record evicts exactly the oldest");

  SimulatorControlJoin restarted("run-b", 42);
  auto post_restart = Record(1, 0.05, 0);
  post_restart.run_id = "run-b";
  restarted.OfferControl(post_restart, started);
  restarted.OfferPhysical({"run-b", 42, 1, 0.05}, started);
  Check(restarted.TakeMatch(1, 0.05).has_value(),
        "new process/run boundary accepts a fresh frame sequence");
  auto pre_restart = Record(2, 0.10, 0);
  restarted.OfferControl(pre_restart, started);
  Check(restarted.diagnostics().wrong_identity_records == 1,
        "pre-restart run identity is rejected after restart");
}

void CheckProjectionTrace() {
  using namespace carla_ego_runtime;
  NormalizedVehicleState state;
  state.run_id = "run-a";
  state.ego_vehicle_id = "42";
  state.frame_id = 88;
  state.simulation_time_s = 4.4;
  state.timestamp_utc = std::chrono::system_clock::time_point{} +
                        std::chrono::milliseconds(4400);
  state.speed_mps = 0.0;
  state.acceleration_iso_mps2 = {0.0, 0.0, 0.0};
  state.throttle_command = 0.0;
  state.brake_command = 1.0;

  SimulatorControlJoin join("run-a", 42);
  const auto now = SimulatorControlJoin::Clock::now();
  join.OfferPhysical({"run-a", 42, 88, 4.4}, now);
  join.OfferControl(Record(88, 4.4, 9, 3, false), now);
  const auto facts = join.TakeMatch(88, 4.4);
  Check(facts.has_value(), "synthetic exact join creates control facts");
  const auto snapshot = ProjectToVss(state, std::nullopt, facts);
  for (const auto &path : {
           "Vehicle.Speed",
           "Vehicle.Chassis.Accelerator.PedalPosition",
           "Vehicle.Chassis.Brake.PedalPosition",
           "Vehicle.CarlaSimulation.FrameId",
           "Vehicle.CarlaSimulation.Control.ActiveMode",
           "Vehicle.CarlaSimulation.Control.TransitionState",
           "Vehicle.CarlaSimulation.Control.Generation",
           "Vehicle.CarlaSimulation.Reset.Generation",
           "Vehicle.CarlaSimulation.Reset.InProgress",
           "Vehicle.CarlaSimulation.Reset.Discontinuity",
       }) {
    const auto *point = Find(snapshot, path);
    Check(point != nullptr, std::string(path) + " is present in the ten-path trace");
    if (point != nullptr) {
      Check(point->timestamp == snapshot.timestamp,
            std::string(path) + " uses the physical frame timestamp");
    }
  }

  SimulatorControlJoin absent("run-a", 42);
  absent.OfferPhysical({"run-a", 42, 89, 4.45}, now);
  const auto missing = ProjectToVss(state, std::nullopt,
                                    absent.TakeMatch(89, 4.45));
  Check(Find(missing, "Vehicle.CarlaSimulation.Control.ActiveMode") == nullptr,
        "missing handoff omits the complete control/reset group");
  Check(Find(missing, "Vehicle.Speed") != nullptr,
        "missing handoff preserves truthful physical telemetry");
}

#if defined(__linux__)
bool SendDatagram(const std::string &path, const std::string &payload) {
  const int descriptor = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(descriptor);
    return false;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto result = ::sendto(
      descriptor, payload.data(), payload.size(), 0,
      reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  ::close(descriptor);
  return result == static_cast<ssize_t>(payload.size());
}

void CheckLinuxTransport() {
  using namespace carla_ego_runtime;
  std::array<char, 64> directory_template{};
  const std::string prefix = "/tmp/carla-control-channel-XXXXXX";
  std::copy(prefix.begin(), prefix.end(), directory_template.begin());
  char *directory = ::mkdtemp(directory_template.data());
  Check(directory != nullptr, "Linux transport test directory is created");
  if (directory == nullptr) {
    return;
  }
  ::chmod(directory, 0700);
  const std::string socket_path = std::string(directory) + "/facts.sock";

  {
    SimulatorControlChannel channel(
        {socket_path, "run-a", 42});
    struct stat status {};
    Check(::lstat(socket_path.c_str(), &status) == 0 &&
              S_ISSOCK(status.st_mode) && (status.st_mode & 0777) == 0600,
          "Linux receiver socket is owner-only");

    Check(SendDatagram(socket_path, Payload(1, 0.05, 1, 0, false)),
          "first producer sends one atomic datagram");
    Check(channel.WaitFor({"run-a", 42, 1, 0.05},
                          std::chrono::milliseconds(50))
              .has_value(),
          "Linux receiver accepts the pinned producer");

    const auto child = ::fork();
    if (child == 0) {
      const bool sent =
          SendDatagram(socket_path, Payload(2, 0.10, 2, 0, false));
      ::_exit(sent ? 0 : 1);
    }
    int child_status = 0;
    Check(child > 0 && ::waitpid(child, &child_status, 0) == child &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "second process sends a credential-bearing test datagram");
    Check(!channel.WaitFor({"run-a", 42, 2, 0.10},
                           std::chrono::milliseconds(20))
               .has_value(),
          "second producer PID is rejected");
    Check(channel.diagnostics().peer_rejections >= 1,
          "peer rejection is counted without changing vehicle state");

    Check(SendDatagram(
              socket_path,
              std::string(kMaximumControlDatagramBytes + 1, 'x')),
          "oversize datagram reaches the receiver atomically");
    Check(!channel.WaitFor({"run-a", 42, 3, 0.15},
                           std::chrono::milliseconds(20))
               .has_value(),
          "oversize datagram never becomes control facts");
    Check(channel.diagnostics().malformed_records >= 1,
          "truncation is counted before JSON processing");
  }
  Check(!std::filesystem::exists(socket_path),
        "receiver removes only its owned socket on shutdown");

  {
    SimulatorControlChannel restarted(
        {socket_path, "run-b", 42});
    auto payload = Payload(1, 0.05, 0, 0, false);
    const auto run = payload.find("run-a");
    payload.replace(run, std::string("run-a").size(), "run-b");
    Check(SendDatagram(socket_path, payload),
          "new run sends through a newly owned socket");
    Check(restarted.WaitFor({"run-b", 42, 1, 0.05},
                            std::chrono::milliseconds(50))
              .has_value(),
          "process restart accepts only the new run boundary");
  }
  ::rmdir(directory);
}
#endif

}  // namespace

int main() {
  CheckDecoder();
  CheckRuntimeOptions();
  CheckJoin();
  CheckProjectionTrace();
#if defined(__linux__)
  CheckLinuxTransport();
#endif
  if (failures == 0) {
    std::cout << "Simulator control channel tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
