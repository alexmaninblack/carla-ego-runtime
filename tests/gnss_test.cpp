#include "carla_ego_runtime/gnss.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void CheckNear(double actual, double expected, double tolerance,
               const std::string &message) {
  Check(std::abs(actual - expected) <= tolerance, message);
}

template <typename Callable>
void CheckThrows(Callable callable, const std::string &message) {
  try {
    callable();
    Check(false, message);
  } catch (const std::exception &) {
  }
}

}  // namespace

int main() {
  using namespace carla_ego_runtime;

  const auto acquired_at = std::chrono::system_clock::time_point{} +
                           std::chrono::seconds(100);
  const CarlaGnssSample sample{42, 10.1, 52.520008, 13.404954, 37.25, acquired_at};
  const auto fix = NormalizeGnssSample(sample);
  Check(fix.source_frame_id == 42, "GNSS source frame retained");
  CheckNear(fix.latitude_deg, 52.520008, 1.0e-12, "latitude retained");
  CheckNear(fix.longitude_deg, 13.404954, 1.0e-12, "longitude retained");
  CheckNear(fix.altitude_m, 37.25, 1.0e-12, "altitude retained");
  Check(fix.timestamp_utc == acquired_at,
        "GNSS preserves UTC captured at its own acquisition");

  auto invalid = sample;
  invalid.latitude_deg = 90.01;
  CheckThrows([&] { NormalizeGnssSample(invalid); },
              "out-of-range latitude rejected");
  invalid = sample;
  invalid.longitude_deg = -180.01;
  CheckThrows([&] { NormalizeGnssSample(invalid); },
              "out-of-range longitude rejected");

  LatestGnssFixStore store;
  Check(!store.LatestFor(42, 10.1, 0.5).has_value(),
        "missing GNSS remains unavailable");
  Check(store.Publish(sample), "first GNSS fix accepted");
  Check(!store.Publish(sample), "duplicate GNSS frame rejected");

  auto older = sample;
  older.frame_id = 41;
  Check(!store.Publish(older), "out-of-order GNSS frame rejected");
  Check(store.publish_count() == 1, "only valid ordered GNSS fix counted");
  Check(store.rejected_count() == 2, "GNSS rejections counted");

  Check(store.LatestFor(42, 10.1, 0.5).has_value(),
        "same-frame GNSS fix available");
  Check(store.LatestFor(44, 10.3, 0.5).has_value(),
        "fresh retained GNSS fix available to later frame");
  Check(NormalizeGnssSample(*store.LatestFor(44, 10.3, 0.5)).timestamp_utc ==
            acquired_at,
        "retained GNSS fix is not retimestamped by a later frame or pause");
  Check(!store.LatestFor(41, 10.05, 0.5).has_value(),
        "future GNSS source frame unavailable");
  Check(!store.LatestFor(60, 10.7, 0.5).has_value(),
        "stale GNSS fix omitted");

  auto malformed = sample;
  malformed.frame_id = 43;
  malformed.altitude_m = std::numeric_limits<double>::quiet_NaN();
  Check(!store.Publish(malformed), "non-finite GNSS callback data rejected");
  Check(store.rejected_count() == 3, "malformed rejection counted");

  CheckThrows([&] { store.LatestFor(44, 10.3, -1.0); },
              "negative freshness threshold rejected");

  if (failures == 0) {
    std::cout << "GNSS tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
