#include "carla_ego_runtime/gnss.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace carla_ego_runtime {
namespace {

void RequireFinite(double value, std::string_view name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void ValidateGnssSample(const CarlaGnssSample &sample) {
  RequireFinite(sample.simulation_time_s, "GNSS simulation_time_s");
  RequireFinite(sample.latitude_deg, "GNSS latitude");
  RequireFinite(sample.longitude_deg, "GNSS longitude");
  RequireFinite(sample.altitude_m, "GNSS altitude");
  if (sample.simulation_time_s < 0.0) {
    throw std::out_of_range("GNSS simulation_time_s must not be negative");
  }
  if (sample.latitude_deg < -90.0 || sample.latitude_deg > 90.0) {
    throw std::out_of_range("GNSS latitude must be in [-90, 90]");
  }
  if (sample.longitude_deg < -180.0 || sample.longitude_deg > 180.0) {
    throw std::out_of_range("GNSS longitude must be in [-180, 180]");
  }
}

}  // namespace

NormalizedGnssFix NormalizeGnssSample(
    const CarlaGnssSample &sample,
    const SimulationClockAnchor &clock_anchor) {
  ValidateGnssSample(sample);
  return {sample.frame_id,
          sample.simulation_time_s,
          clock_anchor.TimestampFor(sample.simulation_time_s),
          sample.latitude_deg,
          sample.longitude_deg,
          sample.altitude_m};
}

bool LatestGnssFixStore::Publish(CarlaGnssSample sample) {
  try {
    ValidateGnssSample(sample);
  } catch (...) {
    std::scoped_lock lock(mutex_);
    ++rejected_count_;
    return false;
  }

  std::scoped_lock lock(mutex_);
  if (latest_.has_value() && sample.frame_id <= latest_->frame_id) {
    ++rejected_count_;
    return false;
  }
  latest_ = sample;
  ++publish_count_;
  return true;
}

std::optional<CarlaGnssSample> LatestGnssFixStore::LatestFor(
    std::uint64_t vehicle_frame_id, double vehicle_simulation_time_s,
    double max_age_seconds) const {
  if (!std::isfinite(vehicle_simulation_time_s) ||
      !std::isfinite(max_age_seconds) || max_age_seconds < 0.0) {
    throw std::invalid_argument("invalid GNSS freshness query");
  }

  std::scoped_lock lock(mutex_);
  if (!latest_.has_value() || latest_->frame_id > vehicle_frame_id) {
    return std::nullopt;
  }
  const double age = vehicle_simulation_time_s - latest_->simulation_time_s;
  constexpr double kTimestampTolerance = 1.0e-9;
  if (age < -kTimestampTolerance || age > max_age_seconds) {
    return std::nullopt;
  }
  return latest_;
}

std::uint64_t LatestGnssFixStore::publish_count() const {
  std::scoped_lock lock(mutex_);
  return publish_count_;
}

std::uint64_t LatestGnssFixStore::rejected_count() const {
  std::scoped_lock lock(mutex_);
  return rejected_count_;
}

}  // namespace carla_ego_runtime
