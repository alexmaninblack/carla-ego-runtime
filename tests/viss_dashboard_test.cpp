#define main dashboard_client_entry
#include "../src/viss_client.cpp"
#undef main

#include <cassert>

int main() {
  Options options;
  MonitorHealth health;
  const auto now = std::chrono::steady_clock::now();
  assert(!DashboardLive(options, health, now));
  health.previous_received_at = now;
  health.last_advancing_frame_at = now;
  health.event_count = 1;
  assert(DashboardLive(options, health, now));
  assert(!DashboardLive(options, health, now + std::chrono::seconds(6)));
  health.previous_received_at = now + std::chrono::seconds(6);
  assert(!DashboardLive(options, health, now + std::chrono::seconds(6)));

  SignalValues signals = {{"Vehicle.CarlaSimulation.Control.ActiveMode", "SAFE_STOP"},
    {"Vehicle.CarlaSimulation.Control.TransitionState", "STABLE"}, {"Vehicle.Speed", "0"},
    {"Vehicle.Chassis.Accelerator.PedalPosition", "0"}, {"Vehicle.Chassis.Brake.PedalPosition", "100"},
    {"Vehicle.CarlaSimulation.Reset.InProgress", "false"}, {"Vehicle.CarlaSimulation.Reset.Discontinuity", "false"},
    {"Vehicle.CarlaSimulation.RunId", "abcdef01-1234-1234-1234-123456789abc"},
    {"Vehicle.CarlaSimulation.Reset.Generation", "2"}};
  assert(StopObservation(signals, true) == "STOPPED (Gateway observation)");
  assert(StopObservation(signals, false) == "UNKNOWN (stale telemetry)");
  signals["Vehicle.CarlaSimulation.Control.ActiveMode"] = "MANUAL";
  assert(StopObservation(signals, true) == "NOT ESTABLISHED");
  signals["Vehicle.CarlaSimulation.Control.ActiveMode"] = "SAFE_STOP";
  signals["Vehicle.Speed"] = "5";
  assert(StopObservation(signals, true) == "NOT ESTABLISHED");
  signals.erase("Vehicle.Chassis.Brake.PedalPosition");
  assert(StopObservation(signals, true) == "UNKNOWN");
  assert(ShortExercise(signals) == "abcdef01 / generation 2");
  assert(VehicleLabel(options, signals) == "Not linked to Demo Control");
  const auto request = BuildMonitorRequest(250);
  assert(request.find("CarlaSimulation.Control.*") != std::string::npos);
  std::ostringstream rendered;
  auto *previous = std::cout.rdbuf(rendered.rdbuf());
  RenderDashboard(options, signals, "now", health, false);
  std::cout.rdbuf(previous);
  assert(rendered.str().find("DISCONNECTED") != std::string::npos);
  assert(rendered.str().find("STALE - last values only") != std::string::npos);
  assert(rendered.str().find("DRIVER ADVISORY") != std::string::npos);
  assert(rendered.str().find("Brake             UNAVAILABLE") != std::string::npos);
  assert(rendered.str().find("Tire              UNAVAILABLE") != std::string::npos);
  std::cout << "Dashboard freshness, stop evidence and unavailable advisory: PASS\n";
}
