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
  options.monitor_json = true;
  std::ostringstream native;
  previous = std::cout.rdbuf(native.rdbuf());
  RenderDashboard(options, signals, "now", health, false);
  std::cout.rdbuf(previous);
  const auto snapshot = json::parse(native.str()).as_object();
  assert(snapshot.at("schemaVersion").as_int64() == 1);
  assert(snapshot.at("state").as_string() == "DISCONNECTED");
  assert(snapshot.at("advisory").as_object().at("brake").as_string() == "UNAVAILABLE");
  assert(snapshot.at("stop").as_string() == "UNKNOWN (stale telemetry)");
  assert(native.str().size() < 65536 && native.str().find('\033') == std::string::npos);
  assert(Parse({"--ca", "fixture", "--monitor-json"}).monitor_json);
  assert(request.find("OEM.BrakeHealth.Advisory.GatewayStatus") != std::string::npos);
  assert(request.find("OEM.TireHealth.Advisory.GatewayStatus") != std::string::npos);
  json::object gateway{{"schemaVersion", 1},
    {"requestId", "11111111-1111-4111-8111-111111111111"},
    {"producerEpoch", "22222222-2222-4222-8222-222222222222"},
    {"sequence", 1}, {"state", "APPLIED"}, {"reason", "NONE"},
    {"gatewayObservedAt", "2026-09-16T09:40:00.000Z"},
    {"activeRecommendation", "INSPECTION_RECOMMENDED"},
    {"activeReasonCode", "PREDICTED_BRAKE_DEGRADATION"},
    {"activeUntil", "2026-09-16T09:40:30.000Z"}};
  // Use the same parser clock so this host test never depends on wall time.
  const auto observed = *ParseIso8601Utc("2026-09-16T09:40:00.000Z");
  const std::string ba="Vehicle.OEM.BrakeHealth.Advisory.Availability",ta="Vehicle.OEM.TireHealth.Advisory.Availability";
  assert(DashboardAdvisory(signals,true,false,observed)=="NOT_AVAILABLE");
  json::object available{{"schemaVersion",1},{"supported",true},{"ready",false},{"everReady",false},
    {"gatewayObservedAt","2026-09-16T09:40:00.000Z"},{"expiresAt","2026-09-16T09:40:15.000Z"}};
  signals[ba]=json::serialize(available);
  assert(DashboardAdvisory(signals,true,false,observed)=="WAITING_FOR_SERVICE");
  available["ready"]=true;available["everReady"]=true;signals[ba]=json::serialize(available);
  assert(DashboardAdvisory(signals,true,false,observed)=="MONITORING");
  available["ready"]=false;signals[ba]=json::serialize(available);
  assert(DashboardAdvisory(signals,true,false,observed)=="UNAVAILABLE");
  available["ready"]=true;signals[ba]=json::serialize(available);signals[ta]=json::serialize(available);
  signals["Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, false, observed) == "INSPECTION_RECOMMENDED");
  signals.erase(ba);
  assert(DashboardAdvisory(signals, true, false, observed) == "INSPECTION_RECOMMENDED");
  available["supported"] = false; signals[ba] = json::serialize(available);
  assert(DashboardAdvisory(signals, true, false, observed) == "INSPECTION_RECOMMENDED");
  available["supported"] = true; signals[ba] = json::serialize(available);
  assert(DashboardAdvisory(signals, true, false, observed + std::chrono::seconds(16)) == "INSPECTION_RECOMMENDED");
  assert(DashboardAdvisory(signals, false, false, observed) == "UNAVAILABLE");
  assert(DashboardAdvisory(signals, true, false, observed + std::chrono::seconds(31)) == "NOT_AVAILABLE");
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "UNAVAILABLE");
  gateway["activeRecommendation"] = "TIRE_INSPECTION_RECOMMENDED";
  gateway["activeReasonCode"] = "PREDICTED_TIRE_WEAR";
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "TIRE_INSPECTION_RECOMMENDED");
  gateway["activeRecommendation"] = "TIRE_REPLACEMENT_RECOMMENDED";
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "TIRE_REPLACEMENT_RECOMMENDED");
  gateway["state"] = "CLEARED"; gateway["activeRecommendation"] = "NONE";
  gateway["activeReasonCode"] = "NONE"; gateway["activeUntil"] = nullptr;
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "MONITORING");
  gateway["state"] = "EXPIRED";
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "MONITORING");
  gateway["extra"] = true;
  signals["Vehicle.OEM.TireHealth.Advisory.GatewayStatus"] = json::serialize(gateway);
  assert(DashboardAdvisory(signals, true, true, observed) == "UNAVAILABLE");
  std::cout << "Dashboard freshness, stop evidence and unavailable advisory: PASS\n";
}
