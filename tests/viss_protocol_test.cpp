#include "carla_ego_runtime/viss_protocol.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace json = boost::json;
using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

json::object ParseObject(const std::string &payload) {
  boost::system::error_code error;
  auto parsed = json::parse(payload, error);
  Check(!error && parsed.is_object(), "response is a JSON object: " + payload);
  return !error && parsed.is_object() ? parsed.as_object() : json::object{};
}

std::string StringAt(const json::object &object, std::string_view key) {
  const auto *value = object.if_contains(key);
  Check(value != nullptr && value->is_string(),
        "response has string field " + std::string(key));
  if (value == nullptr || !value->is_string()) {
    return {};
  }
  return {value->as_string().data(), value->as_string().size()};
}

void PopulateStore(carla_ego_runtime::LatestVssSignalStore &store) {
  using carla_ego_runtime::VssDataPoint;
  using carla_ego_runtime::VssSnapshot;
  VssSnapshot snapshot;
  snapshot.frame_id = 42;
  snapshot.simulation_time_s = 2.1;
  snapshot.timestamp = "2026-08-12T12:34:56.789Z";
  snapshot.data_points = {
      VssDataPoint{"Vehicle.Speed", 36.5, snapshot.timestamp},
      VssDataPoint{"Vehicle.Chassis.Brake.PedalPosition", std::uint64_t{12},
                   snapshot.timestamp},
      VssDataPoint{"Vehicle.CurrentLocation.Latitude", 52.5,
                   "2026-08-12T12:34:56.700Z"},
      VssDataPoint{"Vehicle.CarlaSimulation.RunId", std::string("run-1"),
                   snapshot.timestamp}};
  Check(store.Publish(std::move(snapshot)), "test snapshot published");
}

void CheckError(const carla_ego_runtime::VissResponse &response,
                std::string_view number, std::string_view reason,
                const std::string &message) {
  Check(response.is_error, message + " is marked as an error");
  const auto object = ParseObject(response.payload);
  const auto *error = object.if_contains("error");
  Check(error != nullptr && error->is_object(), message + " has error object");
  if (error != nullptr && error->is_object()) {
    Check(StringAt(error->as_object(), "number") == number,
          message + " has standard number");
    Check(StringAt(error->as_object(), "reason") == reason,
          message + " has standard reason");
    Check(!StringAt(error->as_object(), "description").empty(),
          message + " has description");
  }
}

} // namespace

int main() {
  using carla_ego_runtime::VissProtocolLimits;
  using carla_ego_runtime::VissSessionProtocol;

  carla_ego_runtime::LatestVssSignalStore store;
  PopulateStore(store);
  const auto system_time =
      std::chrono::system_clock::time_point{1723466096789ms};
  const auto steady_time = std::chrono::steady_clock::time_point{1000ms};
  VissSessionProtocol protocol;

  auto response = protocol.HandleRequest(
      R"({"action":"get","path":"Vehicle.Speed","requestId":"get-1"})", store,
      system_time, steady_time);
  Check(!response.is_error, "leaf get succeeds");
  auto object = ParseObject(response.payload);
  Check(StringAt(object, "action") == "get", "get action echoed");
  Check(StringAt(object, "requestId") == "get-1", "request id echoed");
  Check(StringAt(object, "ts").ends_with('Z'), "execution timestamp is UTC");
  const auto &data = object.at("data").as_object();
  Check(StringAt(data, "path") == "Vehicle.Speed", "leaf path returned");
  Check(StringAt(data.at("dp").as_object(), "value") == "36.5",
        "VISS value is serialized as a string");
  Check(StringAt(data.at("dp").as_object(), "ts") == "2026-08-12T12:34:56.789Z",
        "source data timestamp preserved");

  response = protocol.HandleRequest(
      R"({"action":"get","path":"Vehicle","filter":{"variant":"paths","parameter":["Speed","CurrentLocation.*"]},"requestId":"get-2"})",
      store, system_time, steady_time);
  object = ParseObject(response.payload);
  Check(!response.is_error, "paths-filtered branch get succeeds");
  Check(object.at("data").as_array().size() == 2,
        "paths filter selects exact and wildcard paths");

  CheckError(
      protocol.HandleRequest(
          R"({"action":"get","path":"Vehicle.Unknown","requestId":"get-3"})",
          store, system_time, steady_time),
      "404", "unavailable_data", "unknown path");
  CheckError(
      protocol.HandleRequest("not-json", store, system_time, steady_time),
      "400", "bad_request", "malformed JSON");
  CheckError(protocol.HandleRequest(R"({"action":"get","requestId":"get-4"})",
                                    store, system_time, steady_time),
             "400", "bad_request", "missing path");
  CheckError(
      protocol.HandleRequest(
          R"({"action":"set","path":"Vehicle.Speed","value":"0","requestId":"set-1"})",
          store, system_time, steady_time),
      "400", "invalid_data", "read-only set");
  CheckError(
      protocol.HandleRequest(
          R"({"action":"get","path":"Vehicle.Speed","authorization":"token","requestId":"auth-1"})",
          store, system_time, steady_time),
      "404", "unavailable_data", "unsupported authorization");
  CheckError(
      protocol.HandleRequest(
          R"({"action":"get","path":"Vehicle","filter":[{"variant":"paths","parameter":["Speed"]}],"requestId":"get-array-filter"})",
          store, system_time, steady_time),
      "400", "bad_request", "Read rejects a filter array");

  response = protocol.HandleRequest(
      R"({"action":"subscribe","path":"Vehicle","filter":[{"variant":"paths","parameter":["Speed","CurrentLocation.*"]},{"variant":"timebased","parameter":{"period":"100"}}],"requestId":"sub-1"})",
      store, system_time, steady_time);
  Check(!response.is_error, "timebased subscription succeeds");
  object = ParseObject(response.payload);
  const auto subscription_id = StringAt(object, "subscriptionId");
  Check(!subscription_id.empty(), "subscription id returned");
  Check(
      protocol
          .CollectDueSubscriptionEvents(store, system_time, steady_time + 99ms)
          .empty(),
      "subscription does not emit before its period");
  const auto events = protocol.CollectDueSubscriptionEvents(
      store, system_time + 100ms, steady_time + 100ms);
  Check(events.size() == 1, "subscription emits at its period");
  const auto event = ParseObject(events.front());
  Check(StringAt(event, "action") == "subscription",
        "subscription event action");
  Check(StringAt(event, "subscriptionId") == subscription_id,
        "subscription event id");
  Check(event.at("data").as_array().size() == 2,
        "subscription applies path selection");

  response = protocol.HandleRequest(
      std::string(R"({"action":"unsubscribe","subscriptionId":")") +
          subscription_id + R"(","requestId":"unsub-1"})",
      store, system_time + 101ms, steady_time + 101ms);
  Check(!response.is_error, "unsubscribe succeeds");
  object = ParseObject(response.payload);
  Check(object.if_contains("subscriptionId") == nullptr,
        "VISS 3 unsubscribe success omits subscription id");
  Check(protocol
            .CollectDueSubscriptionEvents(store, system_time + 300ms,
                                          steady_time + 300ms)
            .empty(),
        "unsubscribed stream stops");

  CheckError(
      protocol.HandleRequest(
          R"({"action":"subscribe","path":"Vehicle.Speed","filter":{"variant":"timebased","parameter":{"period":"10"}},"requestId":"sub-fast"})",
          store, system_time, steady_time),
      "400", "bad_request", "period below bounded minimum");

  VissProtocolLimits one_subscription;
  one_subscription.max_subscriptions = 1;
  VissSessionProtocol bounded(one_subscription);
  Check(
      !bounded
           .HandleRequest(
               R"({"action":"subscribe","path":"Vehicle.Speed","filter":{"variant":"timebased","parameter":{"period":"50"}},"requestId":"sub-a"})",
               store, system_time, steady_time)
           .is_error,
      "first bounded subscription succeeds");
  CheckError(
      bounded.HandleRequest(
          R"({"action":"subscribe","path":"Vehicle.Speed","filter":{"variant":"timebased","parameter":{"period":"50"}},"requestId":"sub-b"})",
          store, system_time, steady_time),
      "429", "too_many_requests", "subscription cap");

  VissSessionProtocol reconnected;
  CheckError(
      reconnected.HandleRequest(
          R"({"action":"unsubscribe","subscriptionId":"1","requestId":"old-id"})",
          store, system_time, steady_time),
      "404", "unavailable_data", "reconnect clears session state");

  const auto metrics = protocol.metrics();
  Check(metrics.requests >= 11, "request metric counts protocol requests");
  Check(metrics.errors >= 5, "protocol error metric is observable");
  Check(metrics.subscription_events == 1,
        "subscription event metric is observable");

  const auto bounded_before = bounded.metrics();
  const auto coalesced_events = bounded.CollectDueSubscriptionEvents(
      store, system_time + 350ms, steady_time + 350ms);
  const auto bounded_after = bounded.metrics();
  Check(coalesced_events.size() == 1,
        "overdue subscription emits only the latest snapshot");
  Check(bounded_after.coalesced_intervals -
                bounded_before.coalesced_intervals ==
            6,
        "missed subscription intervals are counted");

  if (failures == 0) {
    std::cout << "VISS protocol tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
