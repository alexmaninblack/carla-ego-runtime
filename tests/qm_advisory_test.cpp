#include "carla_ego_runtime/qm_advisory.hpp"
#include "carla_ego_runtime/viss_protocol.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace carla_ego_runtime;
using namespace std::chrono_literals;
namespace json = boost::json;
const std::string brake = "Vehicle.OEM.BrakeHealth.Advisory.Request";
const std::string tire = "Vehicle.OEM.TireHealth.Advisory.Request";
const VissSessionAccess selected{VissClientRole::SelectedPlatformUnit, 1, 0};

std::string Canonical(const json::object &value) {
  std::vector<std::string> keys;
  for (const auto &entry : value) keys.emplace_back(entry.key());
  std::sort(keys.begin(), keys.end());
  json::object sorted;
  for (const auto &key : keys) sorted[key] = value.at(key);
  return json::serialize(sorted);
}
json::object Request(std::chrono::system_clock::time_point now, int sequence = 1, bool is_tire = false) {
  return {{"schemaVersion", 1}, {"requestId", "11111111-1111-4111-8111-111111111111"},
    {"producerEpoch", "22222222-2222-4222-8222-222222222222"}, {"sequence", sequence},
    {"decisionId", "decision-1"}, {"operation", "SET"},
    {"recommendation", is_tire ? "TIRE_INSPECTION_RECOMMENDED" : "INSPECTION_RECOMMENDED"},
    {"reasonCode", is_tire ? "PREDICTED_TIRE_WEAR" : "PREDICTED_BRAKE_DEGRADATION"},
    {"serviceVersion", is_tire ? "29.0.0" : "47.0.0"}, {"modelVersion", "model-1"},
    {"issuedAt", FormatIso8601Utc(now)}, {"expiresAt", FormatIso8601Utc(now + 30s)}};
}
json::object Status(QmAdvisoryGateway &gateway, std::chrono::system_clock::time_point now,
                    std::chrono::steady_clock::time_point mono, bool is_tire = false) {
  return json::parse(std::get<std::string>(gateway.Snapshot(now, mono).at(is_tire ? 3 : 1).value)).as_object();
}
int main() {
  const auto now = std::chrono::system_clock::from_time_t(1789542000);
  const auto mono = std::chrono::steady_clock::time_point(1000s);
  QmAdvisoryGateway gateway(now - 1s);
  assert(gateway.Snapshot(now, mono).at(0).timestamp == FormatIso8601Utc(now));
  auto request = Request(now);
  for (const auto role : {VissClientRole::Development, VissClientRole::EngineeringDashboard,
       VissClientRole::PlatformUpdateRuntime, VissClientRole::QualificationClient})
    assert(gateway.Handle({role, 1, 0}, brake, Canonical(request), now, mono).reason == "UNAUTHORIZED_SOURCE");
  assert(gateway.Handle({VissClientRole::SelectedPlatformUnit, 0, 0}, brake, Canonical(request), now, mono).reason == "UNAUTHORIZED_SOURCE");
  assert(gateway.Handle(selected, "Vehicle.Speed", Canonical(request), now, mono).reason == "UNAUTHORIZED_PATH");
  assert(gateway.Handle(selected, brake + "Extra", Canonical(request), now, mono).reason == "UNAUTHORIZED_PATH");
  assert(gateway.Handle(selected, brake, std::string(2049, 'x'), now, mono).reason == "INVALID_SCHEMA");
  assert(gateway.Handle(selected, brake, " " + Canonical(request), now, mono).reason == "INVALID_SCHEMA");
  auto altered = request;
  altered["functionalProfile"] = "v3";
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "INVALID_SCHEMA");
  altered = request; altered["serviceVersion"] = "v3";
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "INVALID_VALUE");
  altered = request; altered["sequence"] = true;
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "INVALID_VALUE");
  altered = request; altered["recommendation"] = "TIRE_INSPECTION_RECOMMENDED";
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "INVALID_VALUE");
  altered = request; altered["issuedAt"] = "2026-02-31T12:00:00.000Z";
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "INVALID_VALUE");
  altered = request; altered["issuedAt"] = FormatIso8601Utc(now + 1ms);
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "STALE_REQUEST");
  assert(gateway.Handle(selected, brake, Canonical(Request(now - 3s)), now, mono).reason == "STALE_REQUEST");
  altered = request; altered["expiresAt"] = FormatIso8601Utc(now + 31s);
  assert(gateway.Handle(selected, brake, Canonical(altered), now, mono).reason == "STALE_REQUEST");

  assert(gateway.Handle(selected, brake, Canonical(request), now, mono).accepted);
  const auto applied = Status(gateway, now, mono);
  assert(applied.at("state") == "APPLIED" && applied.at("activeRecommendation") == "INSPECTION_RECOMMENDED");
  assert(Canonical(applied).size() <= 1024);
  assert(gateway.Handle(selected, brake, Canonical(request), now + 1s, mono + 1s).reason == "IDEMPOTENT_NO_NEW_EFFECT");
  assert(Status(gateway, now + 1s, mono + 1s) == applied);
  altered = request; altered["serviceVersion"] = "48.0.0";
  assert(gateway.Handle(selected, brake, Canonical(altered), now + 1s, mono + 1s).reason == "REPLAY_DETECTED");
  assert(gateway.Handle(selected, brake, Canonical(request), now + 1s, mono + 1s).reason == "IDEMPOTENT_NO_NEW_EFFECT");
  assert(Status(gateway, now + 1s, mono + 1s) == applied);
  altered = Request(now + 1s, 2);
  assert(gateway.Handle(selected, brake, Canonical(altered), now + 1s, mono + 1s).reason == "RATE_LIMITED");
  altered = Request(now + 10s, 2);
  assert(gateway.Handle(selected, brake, Canonical(altered), now + 10s, mono + 10s).accepted);
  auto rollback = Request(now + 11s, 1); rollback["requestId"] = "33333333-3333-4333-8333-333333333333";
  assert(gateway.Handle(selected, brake, Canonical(rollback), now + 11s, mono + 11s).reason == "SEQUENCE_ROLLBACK");
  auto clear = Request(now + 11s, 3); clear["operation"] = "CLEAR"; clear.erase("recommendation"); clear["reasonCode"] = "CONDITION_CLEARED";
  assert(gateway.Handle(selected, brake, Canonical(clear), now + 11s, mono + 11s).accepted);
  assert(Status(gateway, now + 11s, mono + 11s).at("state") == "CLEARED");
  assert(Status(gateway, now + 11s, mono + 11s).at("activeUntil").is_null());
  const auto tire_request = Request(now, 1, true);
  assert(gateway.Handle(selected, tire, Canonical(tire_request), now, mono).accepted);
  assert(Status(gateway, now + 30s, mono + 30s, true).at("state") == "EXPIRED");
  assert(Status(gateway, now + 30s, mono + 30s, true).at("activeRecommendation") == "NONE");
  assert(gateway.Handle(selected, tire, Canonical(tire_request), now + 30s, mono + 30s).reason == "STALE_REQUEST");
  gateway.ResetAssignment();
  assert(std::get<std::string>(gateway.Snapshot(now, mono).at(1).value).empty());
  QmAdvisoryGateway restarted(now + 1s);
  assert(restarted.Handle(selected, tire, Canonical(tire_request), now + 1s, mono + 1s).reason == "STALE_REQUEST");

  // Shared state survives a WebSocket reconnect; direct Get and combined
  // telemetry subscription both expose the same authoritative status.
  auto shared = std::make_shared<QmAdvisoryGateway>(now - 1s);
  VissSessionProtocol writer({}, selected, shared);
  LatestVssSignalStore store;
  store.Publish({42, 2.1, FormatIso8601Utc(now), {{"Vehicle.Speed", 12.0, FormatIso8601Utc(now)}}});
  const auto set = json::serialize(json::object{{"action", "set"}, {"requestId", "set-1"}, {"path", brake}, {"value", Canonical(request)}});
  assert(!writer.HandleRequest(set, store, now, mono).is_error);
  VissSessionProtocol reader({}, {VissClientRole::EngineeringDashboard, 0, 0}, shared);
  assert(reader.HandleRequest(set, store, now, mono).is_error);
  auto get = reader.HandleRequest(R"({"action":"get","requestId":"read","path":"Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus"})", store, now, mono);
  assert(!get.is_error && get.payload.find("APPLIED") != std::string::npos);
  VissSessionProtocol reconnected({}, selected, shared);
  assert(!reconnected.HandleRequest(set, store, now + 1s, mono + 1s).is_error);
  const auto subscription = R"({"action":"subscribe","requestId":"sub","path":"Vehicle","filter":[{"variant":"paths","parameter":["Speed","OEM.BrakeHealth.Advisory.GatewayStatus","OEM.TireHealth.Advisory.GatewayStatus"]},{"variant":"timebased","parameter":{"period":"250"}}]})";
  assert(!reconnected.HandleRequest(subscription, store, now, mono).is_error);
  const auto events = reconnected.CollectDueSubscriptionEvents(store, now + 250ms, mono + 250ms);
  assert(events.size() == 1 && events[0].find("Vehicle.Speed") != std::string::npos && events[0].find("APPLIED") != std::string::npos);
  assert(!VissRoleMayRead(VissClientRole::PlatformUpdateRuntime, "Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus"));
  // A steady-clock deadline still expires a lease if UTC is adjusted back.
  QmAdvisoryGateway clock_adjusted(now - 1s);
  assert(clock_adjusted.Handle(selected, brake, Canonical(request), now, mono).accepted);
  assert(Status(clock_adjusted, now + 1s, mono + 30s).at("state") == "EXPIRED");
  // Retain replay protection beyond the maximum live lease and reject changed
  // content under an already accepted identity even with new valid UTC dates.
  QmAdvisoryGateway replay_retained(now - 1s);
  assert(replay_retained.Handle(selected, brake, Canonical(request), now, mono).accepted);
  altered = Request(now + 299s);
  assert(replay_retained.Handle(selected, brake, Canonical(altered), now + 299s, mono + 299s).reason == "REPLAY_DETECTED");
  // Exact CLEAR must not smuggle a recommendation or extra arbitrary field.
  clear["recommendation"] = "INSPECTION_RECOMMENDED";
  assert(gateway.Handle(selected, brake, Canonical(clear), now + 11s, mono + 11s).reason == "INVALID_SCHEMA");
  std::cout << "Typed QM Gateway contract and shared VISS status: PASS\n";
}
