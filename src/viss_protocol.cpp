#include "carla_ego_runtime/viss_protocol.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace carla_ego_runtime {
namespace {

namespace json = boost::json;

constexpr std::string_view kBadRequestNumber = "400";
constexpr std::string_view kBadRequestReason = "bad_request";
constexpr std::string_view kInvalidDataReason = "invalid_data";
constexpr std::string_view kUnavailableNumber = "404";
constexpr std::string_view kUnavailableReason = "unavailable_data";
constexpr std::string_view kTooManyRequestsNumber = "429";
constexpr std::string_view kTooManyRequestsReason = "too_many_requests";

std::string AsString(const json::string &value) {
  return {value.data(), value.size()};
}

std::optional<std::string> OptionalString(const json::object &object,
                                          std::string_view key) {
  const auto *value = object.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    return std::nullopt;
  }
  return AsString(value->as_string());
}

std::string ValidResponseAction(const std::optional<std::string> &action) {
  if (action.has_value() &&
      (*action == "get" || *action == "set" || *action == "subscribe" ||
       *action == "unsubscribe")) {
    return *action;
  }
  // A malformed request may not contain an action at all. VISS error
  // responses still require a valid action, so use get as the neutral Read
  // operation while the description identifies the malformed field.
  return "get";
}

VissResponse
ErrorResponse(std::string action, std::string request_id,
              std::string_view number, std::string_view reason,
              std::string_view description,
              std::chrono::system_clock::time_point response_time) {
  json::object error;
  error["number"] = number;
  error["reason"] = reason;
  error["description"] = description;

  json::object response;
  response["action"] = std::move(action);
  response["requestId"] = std::move(request_id);
  response["error"] = std::move(error);
  response["ts"] = FormatIso8601Utc(response_time);
  return {json::serialize(response), true};
}

std::string VssValueString(const VssValue &value) {
  return std::visit(
      [](const auto &item) -> std::string {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::string>) {
          return item;
        } else if constexpr (std::is_same_v<Item, double>) {
          std::ostringstream output;
          output << std::setprecision(std::numeric_limits<double>::max_digits10)
                 << item;
          return output.str();
        } else {
          return std::to_string(item);
        }
      },
      value);
}

json::object DataPointJson(const VssDataPoint &point) {
  json::object dp;
  dp["value"] = VssValueString(point.value);
  dp["ts"] = point.timestamp;

  json::object data;
  data["path"] = point.path;
  data["dp"] = std::move(dp);
  return data;
}

json::value DataJson(const std::vector<const VssDataPoint *> &points) {
  if (points.size() == 1) {
    return DataPointJson(*points.front());
  }
  json::array data;
  data.reserve(points.size());
  for (const auto *point : points) {
    data.emplace_back(DataPointJson(*point));
  }
  return data;
}

bool IsPathWithin(std::string_view path, std::string_view base_path) {
  return path == base_path ||
         (path.size() > base_path.size() && path.starts_with(base_path) &&
          path[base_path.size()] == '.');
}

bool GlobMatches(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_index = std::string_view::npos;
  std::size_t star_text_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() &&
        pattern[pattern_index] == text[text_index]) {
      ++pattern_index;
      ++text_index;
    } else if (pattern_index < pattern.size() &&
               pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      star_text_index = text_index;
    } else if (star_index != std::string_view::npos) {
      pattern_index = star_index + 1;
      text_index = ++star_text_index;
    } else {
      return false;
    }
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

std::vector<const VssDataPoint *>
SelectPoints(const VssSnapshot &snapshot, std::string_view base_path,
             const std::optional<std::vector<std::string>> &relative_patterns) {
  std::vector<const VssDataPoint *> selected;
  for (const auto &point : snapshot.data_points) {
    if (!IsPathWithin(point.path, base_path)) {
      continue;
    }
    if (!relative_patterns.has_value()) {
      selected.push_back(&point);
      continue;
    }

    std::string_view relative;
    if (point.path.size() > base_path.size()) {
      relative = std::string_view(point.path).substr(base_path.size() + 1);
    }
    if (std::any_of(relative_patterns->begin(), relative_patterns->end(),
                    [relative](const std::string &pattern) {
                      return GlobMatches(pattern, relative);
                    })) {
      selected.push_back(&point);
    }
  }
  return selected;
}

bool IsVissValue(const json::value &value) {
  if (value.is_string()) {
    return true;
  }
  if (value.is_array()) {
    return std::all_of(
        value.as_array().begin(), value.as_array().end(),
        [](const json::value &item) { return IsVissValue(item); });
  }
  if (value.is_object()) {
    return std::all_of(
        value.as_object().begin(), value.as_object().end(),
        [](const auto &item) { return IsVissValue(item.value()); });
  }
  return false;
}

struct ParsedFilters {
  std::optional<std::vector<std::string>> paths;
  std::optional<std::chrono::milliseconds> period;
};

bool ParsePeriod(const json::value &parameter, VissProtocolLimits limits,
                 std::chrono::milliseconds &period) {
  if (!parameter.is_object()) {
    return false;
  }
  const auto text = OptionalString(parameter.as_object(), "period");
  if (!text.has_value() || text->empty()) {
    return false;
  }
  std::uint64_t milliseconds = 0;
  const auto result =
      std::from_chars(text->data(), text->data() + text->size(), milliseconds);
  if (result.ec != std::errc{} || result.ptr != text->data() + text->size() ||
      milliseconds > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  period = std::chrono::milliseconds(milliseconds);
  return period >= limits.minimum_period && period <= limits.maximum_period;
}

bool ParsePaths(const json::value &parameter, std::vector<std::string> &paths) {
  if (!parameter.is_array() || parameter.as_array().empty()) {
    return false;
  }
  for (const auto &item : parameter.as_array()) {
    if (!item.is_string() || item.as_string().empty()) {
      return false;
    }
    paths.push_back(AsString(item.as_string()));
  }
  return true;
}

bool ParseOneFilter(const json::value &filter, bool allow_timebased,
                    VissProtocolLimits limits, ParsedFilters &parsed) {
  if (!filter.is_object()) {
    return false;
  }
  const auto &object = filter.as_object();
  const auto variant = OptionalString(object, "variant");
  const auto *parameter = object.if_contains("parameter");
  if (!variant.has_value() || parameter == nullptr) {
    return false;
  }
  if (*variant == "paths" && !parsed.paths.has_value()) {
    std::vector<std::string> paths;
    if (!ParsePaths(*parameter, paths)) {
      return false;
    }
    parsed.paths = std::move(paths);
    return true;
  }
  if (*variant == "timebased" && allow_timebased &&
      !parsed.period.has_value()) {
    std::chrono::milliseconds period;
    if (!ParsePeriod(*parameter, limits, period)) {
      return false;
    }
    parsed.period = period;
    return true;
  }
  return false;
}

bool ParseFilters(const json::value &filter, bool allow_timebased,
                  VissProtocolLimits limits, ParsedFilters &parsed) {
  if (filter.is_array()) {
    if (filter.as_array().empty()) {
      return false;
    }
    for (const auto &item : filter.as_array()) {
      if (!ParseOneFilter(item, allow_timebased, limits, parsed)) {
        return false;
      }
    }
    return true;
  }
  return ParseOneFilter(filter, allow_timebased, limits, parsed);
}

json::object SuccessBase(std::string_view action, std::string_view request_id,
                         std::chrono::system_clock::time_point response_time) {
  json::object response;
  response["action"] = action;
  response["requestId"] = request_id;
  response["ts"] = FormatIso8601Utc(response_time);
  return response;
}

} // namespace

class VissSessionProtocol::Impl {
public:
  explicit Impl(VissProtocolLimits limits) : limits_(limits) {
    if (limits_.max_subscriptions == 0 || limits_.minimum_period.count() <= 0 ||
        limits_.maximum_period < limits_.minimum_period) {
      throw std::invalid_argument("invalid VISS protocol limits");
    }
  }

  VissResponse
  HandleRequest(std::string_view request,
                const LatestVssSignalStore &signal_store,
                std::chrono::system_clock::time_point response_time,
                std::chrono::steady_clock::time_point schedule_time) {
    ++metrics_.requests;
    boost::system::error_code error;
    const auto parsed =
        json::parse(json::string_view(request.data(), request.size()), error);
    if (error || !parsed.is_object()) {
      return ProtocolError("get", "", kBadRequestNumber, kBadRequestReason,
                           "The request is malformed", response_time);
    }

    const auto &object = parsed.as_object();
    const auto action_value = OptionalString(object, "action");
    const auto request_id_value = OptionalString(object, "requestId");
    const auto action = ValidResponseAction(action_value);
    const auto request_id = request_id_value.value_or("");
    if (!action_value.has_value() ||
        (*action_value != "get" && *action_value != "set" &&
         *action_value != "subscribe" && *action_value != "unsubscribe")) {
      return ProtocolError(action, request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid action",
                           response_time);
    }
    if (!request_id_value.has_value() || request_id.empty()) {
      return ProtocolError(action, request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid requestId",
                           response_time);
    }
    if (object.if_contains("dc") != nullptr) {
      return ProtocolError(action, request_id, kUnavailableNumber,
                           kUnavailableReason, "Unsupported feature",
                           response_time);
    }
    if (object.if_contains("authorization") != nullptr) {
      return ProtocolError(
          action, request_id, kUnavailableNumber, kUnavailableReason,
          "Authorization is not supported by this profile", response_time);
    }

    if (action == "get") {
      return HandleGet(object, request_id, signal_store, response_time);
    }
    if (action == "set") {
      return HandleSet(object, request_id, response_time);
    }
    if (action == "subscribe") {
      return HandleSubscribe(object, request_id, signal_store, response_time,
                             schedule_time);
    }
    return HandleUnsubscribe(object, request_id, response_time);
  }

  std::vector<std::string> CollectDueSubscriptionEvents(
      const LatestVssSignalStore &signal_store,
      std::chrono::system_clock::time_point response_time,
      std::chrono::steady_clock::time_point schedule_time) {
    std::vector<std::string> events;
    const auto snapshot = signal_store.Latest();
    for (auto iterator = subscriptions_.begin();
         iterator != subscriptions_.end();) {
      if (schedule_time < iterator->next_due) {
        ++iterator;
        continue;
      }

      const auto overdue = schedule_time - iterator->next_due;
      const auto missed = overdue / iterator->period;
      metrics_.coalesced_intervals += static_cast<std::uint64_t>(missed);
      iterator->next_due += iterator->period * (missed + 1);

      if (!snapshot.has_value()) {
        events.push_back(
            SubscriptionError(*iterator, response_time, "Data is unavailable"));
        iterator = subscriptions_.erase(iterator);
        continue;
      }
      const auto selected =
          SelectPoints(*snapshot, iterator->path, iterator->paths);
      if (selected.empty()) {
        events.push_back(
            SubscriptionError(*iterator, response_time, "Data is unavailable"));
        iterator = subscriptions_.erase(iterator);
        continue;
      }

      json::object event;
      event["action"] = "subscription";
      event["subscriptionId"] = iterator->id;
      event["data"] = DataJson(selected);
      event["ts"] = FormatIso8601Utc(response_time);
      events.push_back(json::serialize(event));
      ++metrics_.subscription_events;
      ++iterator;
    }
    return events;
  }

  VissProtocolMetrics metrics() const { return metrics_; }

private:
  struct Subscription {
    std::string id;
    std::string path;
    std::optional<std::vector<std::string>> paths;
    std::chrono::milliseconds period;
    std::chrono::steady_clock::time_point next_due;
  };

  VissResponse
  ProtocolError(std::string action, std::string request_id,
                std::string_view number, std::string_view reason,
                std::string_view description,
                std::chrono::system_clock::time_point response_time) {
    ++metrics_.errors;
    return ErrorResponse(std::move(action), std::move(request_id), number,
                         reason, description, response_time);
  }

  VissResponse HandleGet(const json::object &object,
                         const std::string &request_id,
                         const LatestVssSignalStore &signal_store,
                         std::chrono::system_clock::time_point response_time) {
    const auto path = OptionalString(object, "path");
    if (!path.has_value() || path->empty()) {
      return ProtocolError("get", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid path",
                           response_time);
    }

    ParsedFilters filters;
    if (const auto *filter = object.if_contains("filter");
        filter != nullptr &&
        (filter->is_array() ||
         !ParseFilters(*filter, false, limits_, filters))) {
      return ProtocolError("get", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid filter",
                           response_time);
    }
    const auto snapshot = signal_store.Latest();
    if (!snapshot.has_value()) {
      return ProtocolError("get", request_id, kUnavailableNumber,
                           kUnavailableReason, "Data is unavailable",
                           response_time);
    }
    const auto selected = SelectPoints(*snapshot, *path, filters.paths);
    if (selected.empty()) {
      return ProtocolError("get", request_id, kUnavailableNumber,
                           kUnavailableReason, "Data is unknown",
                           response_time);
    }

    auto response = SuccessBase("get", request_id, response_time);
    response["data"] = DataJson(selected);
    return {json::serialize(response), false};
  }

  VissResponse HandleSet(const json::object &object,
                         const std::string &request_id,
                         std::chrono::system_clock::time_point response_time) {
    const auto path = OptionalString(object, "path");
    const auto *value = object.if_contains("value");
    if (!path.has_value() || path->empty()) {
      return ProtocolError("set", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid path",
                           response_time);
    }
    if (value == nullptr || !IsVissValue(*value)) {
      return ProtocolError("set", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid value",
                           response_time);
    }
    return ProtocolError("set", request_id, kBadRequestNumber,
                         kInvalidDataReason,
                         "Update of a sensor is not supported", response_time);
  }

  VissResponse
  HandleSubscribe(const json::object &object, const std::string &request_id,
                  const LatestVssSignalStore &signal_store,
                  std::chrono::system_clock::time_point response_time,
                  std::chrono::steady_clock::time_point schedule_time) {
    const auto path = OptionalString(object, "path");
    const auto *filter = object.if_contains("filter");
    if (!path.has_value() || path->empty()) {
      return ProtocolError("subscribe", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid path",
                           response_time);
    }
    ParsedFilters filters;
    if (filter == nullptr || !ParseFilters(*filter, true, limits_, filters) ||
        !filters.period.has_value()) {
      return ProtocolError("subscribe", request_id, kBadRequestNumber,
                           kBadRequestReason, "Missing or invalid filter",
                           response_time);
    }
    if (subscriptions_.size() >= limits_.max_subscriptions) {
      return ProtocolError("subscribe", request_id, kTooManyRequestsNumber,
                           kTooManyRequestsReason, "Subscription limit reached",
                           response_time);
    }
    const auto snapshot = signal_store.Latest();
    if (!snapshot.has_value() ||
        SelectPoints(*snapshot, *path, filters.paths).empty()) {
      return ProtocolError("subscribe", request_id, kUnavailableNumber,
                           kUnavailableReason, "Data is unknown",
                           response_time);
    }

    const auto subscription_id = std::to_string(next_subscription_id_++);
    subscriptions_.push_back({subscription_id, *path, std::move(filters.paths),
                              *filters.period,
                              schedule_time + *filters.period});

    auto response = SuccessBase("subscribe", request_id, response_time);
    response["subscriptionId"] = subscription_id;
    return {json::serialize(response), false};
  }

  VissResponse
  HandleUnsubscribe(const json::object &object, const std::string &request_id,
                    std::chrono::system_clock::time_point response_time) {
    const auto subscription_id = OptionalString(object, "subscriptionId");
    if (!subscription_id.has_value() || subscription_id->empty()) {
      return ProtocolError("unsubscribe", request_id, kBadRequestNumber,
                           kBadRequestReason,
                           "Missing or invalid subscriptionId", response_time);
    }
    const auto iterator =
        std::find_if(subscriptions_.begin(), subscriptions_.end(),
                     [&subscription_id](const Subscription &subscription) {
                       return subscription.id == *subscription_id;
                     });
    if (iterator == subscriptions_.end()) {
      return ProtocolError("unsubscribe", request_id, kUnavailableNumber,
                           kUnavailableReason, "Unknown subscription Id",
                           response_time);
    }
    subscriptions_.erase(iterator);
    auto response = SuccessBase("unsubscribe", request_id, response_time);
    return {json::serialize(response), false};
  }

  std::string
  SubscriptionError(const Subscription &subscription,
                    std::chrono::system_clock::time_point response_time,
                    std::string_view description) {
    json::object error;
    error["number"] = kUnavailableNumber;
    error["reason"] = kUnavailableReason;
    error["description"] = description;
    json::object response;
    response["action"] = "subscription";
    response["subscriptionId"] = subscription.id;
    response["error"] = std::move(error);
    response["ts"] = FormatIso8601Utc(response_time);
    ++metrics_.errors;
    return json::serialize(response);
  }

  VissProtocolLimits limits_;
  VissProtocolMetrics metrics_;
  std::uint64_t next_subscription_id_ = 1;
  std::vector<Subscription> subscriptions_;
};

VissSessionProtocol::VissSessionProtocol(VissProtocolLimits limits)
    : impl_(std::make_unique<Impl>(limits)) {}

VissSessionProtocol::~VissSessionProtocol() = default;
VissSessionProtocol::VissSessionProtocol(VissSessionProtocol &&) noexcept =
    default;
VissSessionProtocol &
VissSessionProtocol::operator=(VissSessionProtocol &&) noexcept = default;

VissResponse VissSessionProtocol::HandleRequest(
    std::string_view request, const LatestVssSignalStore &signal_store,
    std::chrono::system_clock::time_point response_time,
    std::chrono::steady_clock::time_point schedule_time) {
  return impl_->HandleRequest(request, signal_store, response_time,
                              schedule_time);
}

std::vector<std::string> VissSessionProtocol::CollectDueSubscriptionEvents(
    const LatestVssSignalStore &signal_store,
    std::chrono::system_clock::time_point response_time,
    std::chrono::steady_clock::time_point schedule_time) {
  return impl_->CollectDueSubscriptionEvents(signal_store, response_time,
                                             schedule_time);
}

VissProtocolMetrics VissSessionProtocol::metrics() const {
  return impl_->metrics();
}

} // namespace carla_ego_runtime
