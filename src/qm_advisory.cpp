#include "carla_ego_runtime/qm_advisory.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <iomanip>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>

namespace carla_ego_runtime {
namespace {
namespace json = boost::json;
using Wall = std::chrono::system_clock;
using Mono = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::array<std::string_view, 2> kRequestPaths{
    "Vehicle.OEM.BrakeHealth.Advisory.Request",
    "Vehicle.OEM.TireHealth.Advisory.Request"};
constexpr std::array<std::string_view, 2> kStatusPaths{
    "Vehicle.OEM.BrakeHealth.Advisory.GatewayStatus",
    "Vehicle.OEM.TireHealth.Advisory.GatewayStatus"};
constexpr std::array<std::string_view,2> kAvailabilityPaths{
    "Vehicle.OEM.BrakeHealth.Advisory.Availability",
    "Vehicle.OEM.TireHealth.Advisory.Availability"};

std::string Text(const json::object &value, std::string_view name) {
  const auto *item = value.if_contains(name);
  return item && item->is_string() ? std::string(item->as_string()) : "";
}

std::optional<std::uint64_t> Integer(const json::value *value) {
  if (!value) return std::nullopt;
  if (value->is_uint64()) return value->as_uint64();
  if (value->is_int64() && value->as_int64() >= 0)
    return static_cast<std::uint64_t>(value->as_int64());
  return std::nullopt;
}

// All keys and schema values are ASCII strings or positive integers, so the
// RFC 8785 representation is the sorted-key, unescaped JSON serialization.
std::string Canonical(const json::object &value) {
  std::vector<std::string> names;
  for (const auto &entry : value) names.emplace_back(entry.key());
  std::sort(names.begin(), names.end());
  json::object sorted;
  for (const auto &name : names) sorted[name] = value.at(name);
  return json::serialize(sorted);
}

bool Match(std::string_view value, const char *pattern) {
  return std::regex_match(value.begin(), value.end(), std::regex(pattern));
}

bool Uuid(std::string_view value) {
  return Match(value, "[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}");
}

std::optional<Wall::time_point> Timestamp(std::string_view text) {
  // Match the UTC date-time forms emitted by the accepted service producers.
  // Do not normalize invalid calendar dates (timegm alone would do so).
  if (!Match(text, "[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,6})?Z"))
    return std::nullopt;
  std::tm value{};
  std::istringstream stream{std::string(text.substr(0, 19))};
  stream >> std::get_time(&value, "%Y-%m-%dT%H:%M:%S");
  if (stream.fail()) return std::nullopt;
  const auto original = value;
  const auto seconds = timegm(&value);
  if (seconds < 0 || value.tm_year != original.tm_year ||
      value.tm_mon != original.tm_mon || value.tm_mday != original.tm_mday ||
      value.tm_hour != original.tm_hour || value.tm_min != original.tm_min ||
      value.tm_sec != original.tm_sec) return std::nullopt;
  unsigned microseconds = 0;
  if (text[19] == '.') {
    const auto fraction = text.substr(20, text.size() - 21);
    const auto result = std::from_chars(fraction.data(), fraction.data() + fraction.size(), microseconds);
    if (result.ec != std::errc{}) return std::nullopt;
    for (std::size_t index = fraction.size(); index < 6; ++index) microseconds *= 10;
  }
  return Wall::from_time_t(seconds) + std::chrono::microseconds(microseconds);
}

struct Request {
  json::object value;
  std::string raw;
  std::string id;
  std::string epoch;
  std::uint64_t sequence = 0;
  std::string operation;
  std::string recommendation;
  std::string reason;
  Wall::time_point issued;
  Wall::time_point expires;
};

std::string Parse(std::string_view raw, std::size_t endpoint, Request &out) {
  if (raw.empty() || raw.size() > 2048) return "INVALID_SCHEMA";
  boost::system::error_code error;
  auto value = json::parse(raw, error);
  if (error || !value.is_object() || Canonical(value.as_object()) != raw)
    return "INVALID_SCHEMA";
  out.value = std::move(value.as_object());
  const auto &object = out.value;
  out.operation = Text(object, "operation");
  const std::array<std::string_view, 11> required{
      "schemaVersion", "requestId", "producerEpoch", "sequence", "operation",
      "reasonCode", "decisionId", "serviceVersion", "modelVersion", "issuedAt", "expiresAt"};
  if (object.size() != required.size() + (out.operation == "SET" ? 1 : 0) ||
      !std::all_of(required.begin(), required.end(), [&](auto name) { return object.contains(name); }) ||
      Integer(object.if_contains("schemaVersion")) != 1)
    return "INVALID_SCHEMA";
  out.id = Text(object, "requestId");
  out.epoch = Text(object, "producerEpoch");
  const auto sequence = Integer(object.if_contains("sequence"));
  if (!Uuid(out.id) || !Uuid(out.epoch) || !sequence || *sequence == 0 ||
      *sequence > 9007199254740991ULL ||
      !Match(Text(object, "decisionId"), "[A-Za-z0-9._:-]{1,96}") ||
      !Match(Text(object, "modelVersion"), "[A-Za-z0-9._-]{1,64}") ||
      !Match(Text(object, "serviceVersion"), "[0-9]+\\.[0-9]+\\.[0-9]+"))
    return "INVALID_VALUE";
  out.sequence = *sequence;
  out.reason = Text(object, "reasonCode");
  out.recommendation = Text(object, "recommendation");
  if (out.operation == "SET") {
    if (endpoint == 0 && (out.recommendation != "INSPECTION_RECOMMENDED" || out.reason != "PREDICTED_BRAKE_DEGRADATION"))
      return "INVALID_VALUE";
    if (endpoint == 1 && ((out.recommendation != "TIRE_INSPECTION_RECOMMENDED" && out.recommendation != "TIRE_REPLACEMENT_RECOMMENDED") || out.reason != "PREDICTED_TIRE_WEAR"))
      return "INVALID_VALUE";
  } else if (out.operation != "CLEAR" || out.reason != "CONDITION_CLEARED" || object.contains("recommendation")) {
    return "INVALID_VALUE";
  }
  const auto issued = Timestamp(Text(object, "issuedAt"));
  const auto expires = Timestamp(Text(object, "expiresAt"));
  if (!issued || !expires) return "INVALID_VALUE";
  out.issued = *issued;
  out.expires = *expires;
  out.raw = raw;
  return {};
}
} // namespace

bool IsQmAdvisoryRequestPath(std::string_view path) {
  return std::find(kRequestPaths.begin(), kRequestPaths.end(), path) != kRequestPaths.end();
}
bool IsQmAdvisoryPath(std::string_view path) {
  return IsQmAdvisoryRequestPath(path) || IsQmAdvisoryAvailabilityPath(path) ||
         std::find(kStatusPaths.begin(), kStatusPaths.end(), path) != kStatusPaths.end();
}
bool IsQmAdvisoryAvailabilityPath(std::string_view path) {
  return std::find(kAvailabilityPaths.begin(),kAvailabilityPaths.end(),path)!=kAvailabilityPaths.end();
}

class QmAdvisoryGateway::Impl {
public:
  explicit Impl(Wall::time_point started) : started_(started) {}

  QmAdvisoryResult Handle(VissSessionAccess access, std::string_view path,
                          std::string_view raw, Wall::time_point now, Mono::time_point mono) {
    std::lock_guard lock(mutex_);
    if (access.role != VissClientRole::SelectedPlatformUnit || access.assignment_generation == 0)
      return {false, "UNAUTHORIZED_SOURCE"};
    const auto availability=std::find(kAvailabilityPaths.begin(),kAvailabilityPaths.end(),path);
    if(availability!=kAvailabilityPaths.end()) {
      if(raw.empty()||raw.size()>256)return {false,"INVALID_SCHEMA"};
      boost::system::error_code error;const auto value=json::parse(raw,error);
      if(error||!value.is_object()||Canonical(value.as_object())!=raw)return {false,"INVALID_SCHEMA"};
      const auto& fields=value.as_object();const auto* ready=fields.if_contains("ready");
      if(fields.size()!=3||Integer(fields.if_contains("schemaVersion"))!=1||!ready||!ready->is_bool())
        return {false,"INVALID_SCHEMA"};
      const auto observed=Timestamp(Text(fields,"observedAt"));
      // Display readiness is not a motion/update authorization. Tolerate the
      // accepted demo clock skew; expiry still uses our own receipt clock.
      if(!observed||*observed<started_||*observed>now+5s||now-*observed>15s)return {false,"STALE_REQUEST"};
      auto& target=endpoints_[static_cast<std::size_t>(availability-kAvailabilityPaths.begin())];
      if(target.support_observed && *observed<=*target.support_observed)return {false,"STALE_REQUEST"};
      target.support_observed=*observed;target.support_received=now;target.support_deadline=mono+15s;
      target.producer_ready=ready->as_bool();target.producer_seen=target.producer_seen||target.producer_ready;
      return {true,"NONE"};
    }
    const auto found = std::find(kRequestPaths.begin(), kRequestPaths.end(), path);
    if (found == kRequestPaths.end()) return {false, "UNAUTHORIZED_PATH"};
    const auto index = static_cast<std::size_t>(found - kRequestPaths.begin());
    auto &endpoint = endpoints_[index];
    Expire(endpoint, now, mono);
    Request request;
    const auto invalid = Parse(raw, index, request);
    if (!invalid.empty()) return {false, invalid};
    auto reject = [&](const std::string &reason) {
      endpoint.status = Status(endpoint, request, "REJECTED", reason, now);
      return QmAdvisoryResult{false, reason};
    };
    // No durable replay store is invented. Previously issued targets cannot
    // acquire a new effect after a Gateway process restart.
    if (request.issued < started_ || request.issued > now + 100ms || now - request.issued > 2s ||
        request.expires <= now || request.expires <= request.issued || request.expires - request.issued > 30s)
      return reject("STALE_REQUEST");
    while (!endpoint.replay.empty() && mono - endpoint.replay.front().accepted_at >= 300s)
      endpoint.replay.pop_front();
    for (const auto &previous : endpoint.replay) {
      if (previous.request.id == request.id && previous.request.epoch == request.epoch && previous.request.sequence == request.sequence) {
        if (previous.request.raw != raw) return reject("REPLAY_DETECTED");
        // A delayed duplicate of an older request must not replace the current
        // status or renew the active lease.
        if (endpoint.last_accepted && endpoint.last_accepted->raw == raw &&
            (endpoint.active || request.operation == "CLEAR"))
          endpoint.status = endpoint.last_application_status;
        return {true, "IDEMPOTENT_NO_NEW_EFFECT"};
      }
      if (previous.request.epoch == request.epoch && previous.request.sequence >= request.sequence)
        return reject("SEQUENCE_ROLLBACK");
    }
    if (endpoint.last_accepted) {
      const bool same = request.operation == endpoint.last_accepted->operation &&
                        request.recommendation == endpoint.last_accepted->recommendation;
      if (mono - endpoint.last_accepted_at < (same ? 10s : 1s)) return reject("RATE_LIMITED");
    }
    if (endpoint.replay.size() >= 512) return reject("RATE_LIMITED");
    endpoint.replay.push_back({request, mono});
    endpoint.last_accepted = request;
    endpoint.last_accepted_at = mono;
    endpoint.request = raw;
    endpoint.request_timestamp = FormatIso8601Utc(now);
    endpoint.active = request.operation == "SET" ? std::optional<Request>(request) : std::nullopt;
    // A tolerated positive clock offset must not enlarge the warning lease.
    // Keep the original envelope intact for provenance and replay comparison.
    endpoint.active_until = std::min(request.expires, now + 30s);
    endpoint.deadline = mono + (endpoint.active_until - now);
    endpoint.status = Status(endpoint, request, request.operation == "SET" ? "APPLIED" : "CLEARED", "NONE", now);
    endpoint.last_application_status = endpoint.status;
    return {true, "NONE"};
  }

  std::vector<VssDataPoint> Snapshot(Wall::time_point now, Mono::time_point mono) {
    std::lock_guard lock(mutex_);
    std::vector<VssDataPoint> result;
    for (std::size_t index = 0; index < endpoints_.size(); ++index) {
      auto &endpoint = endpoints_[index];
      Expire(endpoint, now, mono);
      // Register exact leaves without inventing a status identity before the
      // first request. Empty values mean unavailable, never application proof.
      result.push_back({std::string(kRequestPaths[index]), endpoint.request,
                        endpoint.request_timestamp.empty() ? FormatIso8601Utc(now) : endpoint.request_timestamp});
      result.push_back({std::string(kStatusPaths[index]), endpoint.status, FormatIso8601Utc(now)});
    }
    // Append the readiness projection without changing legacy leaf ordering.
    for(std::size_t index=0;index<endpoints_.size();++index) {
      const auto& e=endpoints_[index];std::string projection;
      if(e.support_observed) {
        const bool supported=mono<e.support_deadline&&now<e.support_received+15s;
        projection=Canonical(json::object{{"schemaVersion",1},{"supported",supported},
          {"ready",supported&&e.producer_ready},{"everReady",e.producer_seen},
          {"gatewayObservedAt",FormatIso8601Utc(e.support_received)},{"expiresAt",FormatIso8601Utc(e.support_received+15s)}});
      }
      result.push_back({std::string(kAvailabilityPaths[index]),projection,FormatIso8601Utc(now)});
    }
    return result;
  }

  void Reset() {
    std::lock_guard lock(mutex_);
    // Source handover must not carry an advisory to the next vehicle.
    endpoints_ = {};
  }

private:
  struct Replay { Request request; Mono::time_point accepted_at; };
  struct Endpoint {
    std::optional<Request> active;
    std::optional<Request> last_accepted;
    Mono::time_point last_accepted_at;
    Mono::time_point deadline;
    Wall::time_point active_until;
    std::deque<Replay> replay;
    std::string request;
    std::string request_timestamp;
    std::string status;
    std::string last_application_status;
    std::optional<Wall::time_point> support_observed;
    Wall::time_point support_received;
    Mono::time_point support_deadline;
    bool producer_ready=false,producer_seen=false;
  };
  static std::string Status(const Endpoint &endpoint, const Request &request,
                            std::string_view state, std::string_view reason, Wall::time_point now) {
    json::object status{{"schemaVersion", 1}, {"requestId", request.id},
        {"producerEpoch", request.epoch}, {"sequence", request.sequence},
        {"state", state}, {"reason", reason}, {"gatewayObservedAt", FormatIso8601Utc(now)},
        {"activeRecommendation", endpoint.active ? endpoint.active->recommendation : "NONE"},
        {"activeReasonCode", endpoint.active ? endpoint.active->reason : "NONE"},
        {"activeUntil", endpoint.active ? json::value(FormatIso8601Utc(endpoint.active_until)) : json::value(nullptr)}};
    return Canonical(status);
  }
  static void Expire(Endpoint &endpoint, Wall::time_point now, Mono::time_point mono) {
    if (endpoint.active && (mono >= endpoint.deadline || now >= endpoint.active_until)) {
      const auto expired = *endpoint.active;
      endpoint.active.reset();
      endpoint.status = Status(endpoint, expired, "EXPIRED", "NONE", now);
    }
  }
  Wall::time_point started_;
  std::array<Endpoint, 2> endpoints_;
  std::mutex mutex_;
};

QmAdvisoryGateway::QmAdvisoryGateway(Wall::time_point started_at)
    : impl_(std::make_unique<Impl>(started_at)) {}
QmAdvisoryGateway::~QmAdvisoryGateway() = default;
QmAdvisoryResult QmAdvisoryGateway::Handle(VissSessionAccess access, std::string_view path,
                                        std::string_view raw, Wall::time_point now, Mono::time_point mono) {
  return impl_->Handle(access, path, raw, now, mono);
}
std::vector<VssDataPoint> QmAdvisoryGateway::Snapshot(Wall::time_point now, Mono::time_point mono) {
  return impl_->Snapshot(now, mono);
}
void QmAdvisoryGateway::ResetAssignment() { impl_->Reset(); }

} // namespace carla_ego_runtime
