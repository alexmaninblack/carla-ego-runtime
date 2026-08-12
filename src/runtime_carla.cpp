#include "carla_ego_runtime/runtime.hpp"
#include "carla_ego_runtime/gnss.hpp"
#include "carla_ego_runtime/vehicle_state.hpp"
#include "carla_ego_runtime/vss.hpp"

#include <carla/client/Actor.h>
#include <carla/client/ActorAttribute.h>
#include <carla/client/ActorBlueprint.h>
#include <carla/client/ActorList.h>
#include <carla/client/BlueprintLibrary.h>
#include <carla/client/Client.h>
#include <carla/client/Map.h>
#include <carla/client/Sensor.h>
#include <carla/client/ServerSideSensor.h>
#include <carla/client/TimeoutException.h>
#include <carla/client/Vehicle.h>
#include <carla/client/World.h>
#include <carla/geom/Location.h>
#include <carla/geom/Rotation.h>
#include <carla/geom/Transform.h>
#include <carla/trafficmanager/TrafficManager.h>
#include <carla/sensor/SensorData.h>
#include <carla/sensor/data/GnssMeasurement.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace carla_ego_runtime {
namespace {

namespace cc = carla::client;

volatile std::sig_atomic_t stop_requested = 0;

void RequestStop(int) {
  stop_requested = 1;
}

class OwnedActorGuard {
 public:
  explicit OwnedActorGuard(carla::SharedPtr<cc::Actor> actor)
      : actor_(std::move(actor)) {}

  OwnedActorGuard(const OwnedActorGuard &) = delete;
  OwnedActorGuard &operator=(const OwnedActorGuard &) = delete;

  ~OwnedActorGuard() {
    DestroyNoThrow();
  }

  bool Destroy() {
    if (!actor_) {
      return true;
    }
    const auto actor_id = actor_->GetId();
    const bool destroyed = !actor_->IsAlive() || actor_->Destroy();
    if (destroyed) {
      std::cout << "Destroyed runtime-owned ego vehicle id=" << actor_id << '\n';
      actor_.reset();
    }
    return destroyed;
  }

 private:
  void DestroyNoThrow() noexcept {
    if (!actor_) {
      return;
    }
    try {
      if (!Destroy()) {
        std::cerr << "Failed to destroy runtime-owned ego vehicle id="
                  << actor_->GetId() << '\n';
      }
    } catch (const std::exception &error) {
      std::cerr << "Failed to destroy runtime-owned ego vehicle: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Failed to destroy runtime-owned ego vehicle: unknown error\n";
    }
  }

  carla::SharedPtr<cc::Actor> actor_;
};

class WorldSettingsGuard {
 public:
  explicit WorldSettingsGuard(cc::World &world)
      : world_(world), original_(world.GetSettings()) {}

  WorldSettingsGuard(const WorldSettingsGuard &) = delete;
  WorldSettingsGuard &operator=(const WorldSettingsGuard &) = delete;

  ~WorldSettingsGuard() {
    RestoreNoThrow();
  }

  void EnableSynchronousMode(double fixed_delta_seconds,
                             std::chrono::milliseconds timeout) {
    auto settings = original_;
    settings.synchronous_mode = true;
    settings.fixed_delta_seconds = fixed_delta_seconds;
    world_.ApplySettings(settings, timeout);
    active_ = true;
  }

  void Restore() {
    if (!active_) {
      return;
    }
    world_.ApplySettings(original_, std::chrono::seconds(10));
    active_ = false;
  }

 private:
  void RestoreNoThrow() noexcept {
    try {
      Restore();
    } catch (const std::exception &error) {
      std::cerr << "Failed to restore CARLA world settings: " << error.what()
                << '\n';
    } catch (...) {
      std::cerr << "Failed to restore CARLA world settings: unknown error\n";
    }
  }

  cc::World &world_;
  carla::rpc::EpisodeSettings original_;
  bool active_ = false;
};

class TrafficManagerGuard {
 public:
  TrafficManagerGuard(carla::traffic_manager::TrafficManager &traffic_manager,
                      carla::SharedPtr<cc::Vehicle> vehicle)
      : traffic_manager_(traffic_manager), vehicle_(std::move(vehicle)) {
    traffic_manager_.SetSynchronousMode(true);
    traffic_manager_.SetRandomDeviceSeed(42);
    traffic_manager_.SetPercentageSpeedDifference(vehicle_, 35.0f);
    vehicle_->SetAutopilot(true, traffic_manager_.Port());
    active_ = true;
  }

  TrafficManagerGuard(const TrafficManagerGuard &) = delete;
  TrafficManagerGuard &operator=(const TrafficManagerGuard &) = delete;

  ~TrafficManagerGuard() { RestoreNoThrow(); }

  void Restore() {
    if (!active_) {
      return;
    }
    vehicle_->SetAutopilot(false, traffic_manager_.Port());
    traffic_manager_.SetSynchronousMode(false);
    active_ = false;
  }

 private:
  void RestoreNoThrow() noexcept {
    try {
      Restore();
    } catch (const std::exception &error) {
      std::cerr << "Failed to restore Traffic Manager state: " << error.what()
                << '\n';
    } catch (...) {
      std::cerr << "Failed to restore Traffic Manager state: unknown error\n";
    }
  }

  carla::traffic_manager::TrafficManager &traffic_manager_;
  carla::SharedPtr<cc::Vehicle> vehicle_;
  bool active_ = false;
};

class OwnedSensorGuard {
 public:
  explicit OwnedSensorGuard(carla::SharedPtr<cc::Sensor> sensor)
      : sensor_(std::move(sensor)) {}

  OwnedSensorGuard(const OwnedSensorGuard &) = delete;
  OwnedSensorGuard &operator=(const OwnedSensorGuard &) = delete;

  ~OwnedSensorGuard() { DestroyNoThrow(); }

  bool Destroy() {
    if (!sensor_) {
      return true;
    }
    if (sensor_->IsListening()) {
      sensor_->Stop();
    }
    const auto actor_id = sensor_->GetId();
    const bool destroyed = !sensor_->IsAlive() || sensor_->Destroy();
    if (destroyed) {
      std::cout << "Destroyed runtime-owned GNSS sensor id=" << actor_id
                << '\n';
      sensor_.reset();
    }
    return destroyed;
  }

 private:
  void DestroyNoThrow() noexcept {
    try {
      if (!Destroy()) {
        std::cerr << "Failed to destroy runtime-owned GNSS sensor\n";
      }
    } catch (const std::exception &error) {
      std::cerr << "Failed to destroy runtime-owned GNSS sensor: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Failed to destroy runtime-owned GNSS sensor: unknown error\n";
    }
  }

  carla::SharedPtr<cc::Sensor> sensor_;
};

bool HasRoleName(const cc::Actor &actor, const std::string &role_name) {
  for (const auto &attribute : actor.GetAttributes()) {
    if (attribute.GetId() == "role_name" &&
        attribute.As<std::string>() == role_name) {
      return true;
    }
  }
  return false;
}

carla::SharedPtr<cc::Vehicle> FindEgoVehicle(const cc::World &world,
                                             const std::string &role_name) {
  const auto actors = world.GetActors();
  if (!actors) {
    throw std::runtime_error("CARLA returned no actor list");
  }

  const auto vehicles = actors->Filter("vehicle.*");
  if (!vehicles) {
    throw std::runtime_error("CARLA returned no filtered vehicle list");
  }

  for (const auto &actor : *vehicles) {
    if (actor && HasRoleName(*actor, role_name)) {
      auto vehicle = std::dynamic_pointer_cast<cc::Vehicle>(actor);
      if (!vehicle) {
        throw std::runtime_error("actor with requested role is not a vehicle");
      }
      return vehicle;
    }
  }
  return nullptr;
}

carla::SharedPtr<cc::Vehicle> SpawnEgoVehicle(cc::World &world,
                                              const RuntimeOptions &options) {
  const auto blueprints = world.GetBlueprintLibrary();
  if (!blueprints) {
    throw std::runtime_error("CARLA returned no blueprint library");
  }

  const auto *blueprint_definition = blueprints->Find(options.blueprint_id);
  if (blueprint_definition == nullptr) {
    throw std::runtime_error("vehicle blueprint not found: " +
                             options.blueprint_id);
  }

  auto blueprint = *blueprint_definition;
  if (!blueprint.ContainsAttribute("role_name")) {
    throw std::runtime_error("vehicle blueprint has no role_name attribute: " +
                             options.blueprint_id);
  }
  blueprint.SetAttribute("role_name", options.role_name);

  const auto map = world.GetMap();
  if (!map) {
    throw std::runtime_error("CARLA returned no map");
  }
  const auto &spawn_points = map->GetRecommendedSpawnPoints();
  if (spawn_points.empty()) {
    throw std::runtime_error("current CARLA map has no recommended spawn points");
  }

  const auto first = options.spawn_point_index % spawn_points.size();
  for (std::size_t attempt = 0; attempt < spawn_points.size(); ++attempt) {
    const auto index = (first + attempt) % spawn_points.size();
    auto actor = world.TrySpawnActor(blueprint, spawn_points[index]);
    if (!actor) {
      continue;
    }

    auto vehicle = std::dynamic_pointer_cast<cc::Vehicle>(actor);
    if (!vehicle) {
      actor->Destroy();
      throw std::runtime_error("spawned ego actor is not a CARLA vehicle");
    }
    std::cout << "Spawn point index: " << index << '\n';
    return vehicle;
  }

  throw std::runtime_error("all recommended spawn points are occupied");
}

std::string GenerateRunId() {
  std::random_device random;
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (int index = 0; index < 4; ++index) {
    output << std::setw(8) << static_cast<std::uint32_t>(random());
  }
  return output.str();
}

void ConfigureStopSignals() {
  stop_requested = 0;
  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);
}

bool ReachedStopCondition(const RuntimeOptions &options,
                          std::uint64_t frame_count,
                          std::chrono::steady_clock::time_point started_at) {
  if (stop_requested != 0) {
    return true;
  }
  if (options.max_frames != 0 && frame_count >= options.max_frames) {
    return true;
  }
  return options.run_seconds != 0 &&
         std::chrono::steady_clock::now() >=
             started_at + std::chrono::seconds(options.run_seconds);
}

carla::geom::Transform ChaseCameraTransform(const cc::Vehicle &vehicle) {
  const auto vehicle_transform = vehicle.GetTransform();
  const auto forward = vehicle_transform.GetForwardVector();
  carla::geom::Location location{
      vehicle_transform.location.x - 8.0f * forward.x,
      vehicle_transform.location.y - 8.0f * forward.y,
      vehicle_transform.location.z + 3.5f};
  carla::geom::Rotation rotation{-12.0f, vehicle_transform.rotation.yaw, 0.0f};
  return {location, rotation};
}

std::string FormatSensorTick(double seconds) {
  std::ostringstream output;
  output << std::setprecision(std::numeric_limits<double>::max_digits10)
         << seconds;
  return output.str();
}

carla::SharedPtr<cc::Sensor> SpawnGnssSensor(
    cc::World &world, cc::Vehicle &vehicle,
    double sensor_tick_seconds) {
  const auto blueprints = world.GetBlueprintLibrary();
  if (!blueprints) {
    throw std::runtime_error("CARLA returned no blueprint library for GNSS");
  }
  const auto *definition = blueprints->Find("sensor.other.gnss");
  if (!definition) {
    throw std::runtime_error("CARLA GNSS blueprint is unavailable");
  }
  auto blueprint = *definition;
  if (!blueprint.ContainsAttribute("sensor_tick")) {
    throw std::runtime_error("CARLA GNSS blueprint has no sensor_tick");
  }
  blueprint.SetAttribute("sensor_tick", FormatSensorTick(sensor_tick_seconds));

  carla::geom::Transform relative_transform{
      carla::geom::Location{0.0f, 0.0f, 2.0f},
      carla::geom::Rotation{0.0f, 0.0f, 0.0f}};
  auto actor = world.SpawnActor(blueprint, relative_transform, &vehicle,
                                carla::rpc::AttachmentType::Rigid);
  auto sensor = std::dynamic_pointer_cast<cc::Sensor>(actor);
  if (!sensor) {
    actor->Destroy();
    throw std::runtime_error("spawned GNSS actor is not a CARLA sensor");
  }
  return sensor;
}

CarlaVehicleSample CollectSample(
    const cc::WorldSnapshot &snapshot, cc::Vehicle &vehicle,
    const std::string &run_id, const SimulationClockAnchor &clock_anchor) {
  const auto actor_snapshot = snapshot.Find(vehicle.GetId());
  if (!actor_snapshot.has_value()) {
    throw std::runtime_error("ego vehicle is absent from world snapshot frame " +
                             std::to_string(snapshot.GetFrame()));
  }

  auto acceleration_vehicle = actor_snapshot->acceleration;
  actor_snapshot->transform.rotation.InverseRotateVector(acceleration_vehicle);
  const auto telemetry = vehicle.GetTelemetryData();

  CarlaVehicleSample sample;
  sample.run_id = run_id;
  sample.ego_vehicle_id = std::to_string(vehicle.GetId());
  sample.frame_id = static_cast<std::uint64_t>(snapshot.GetFrame());
  sample.simulation_time_s = snapshot.GetTimestamp().elapsed_seconds;
  sample.timestamp_utc = clock_anchor.TimestampFor(sample.simulation_time_s);
  sample.velocity_world_mps = {actor_snapshot->velocity.x,
                               actor_snapshot->velocity.y,
                               actor_snapshot->velocity.z};
  sample.acceleration_vehicle_carla_mps2 = {
      acceleration_vehicle.x, acceleration_vehicle.y, acceleration_vehicle.z};
  sample.throttle_command = telemetry.throttle;
  sample.brake_command = telemetry.brake;
  sample.steering_command = telemetry.steer;
  sample.gear = telemetry.gear;
  sample.engine_rpm = telemetry.engine_rpm;
  sample.front_left_wheel_angle_carla_deg = vehicle.GetWheelSteerAngle(
      cc::Vehicle::WheelLocation::FL_Wheel);
  sample.front_right_wheel_angle_carla_deg = vehicle.GetWheelSteerAngle(
      cc::Vehicle::WheelLocation::FR_Wheel);
  return sample;
}

void CollectVehicleState(cc::Client &client, cc::World &world,
                         carla::SharedPtr<cc::Vehicle> vehicle,
                         const RuntimeOptions &options) {
  const auto timeout = std::chrono::milliseconds(options.timeout_ms);
  WorldSettingsGuard settings_guard(world);
  if (options.tick_owner) {
    settings_guard.EnableSynchronousMode(options.fixed_delta_seconds, timeout);
    std::cout << "Synchronous tick owner: yes (fixed delta "
              << options.fixed_delta_seconds << " s)\n";
  } else {
    std::cout << "Synchronous tick owner: no (observing external ticks)\n";
  }

  std::optional<carla::traffic_manager::TrafficManager> traffic_manager;
  std::optional<TrafficManagerGuard> traffic_manager_guard;
  if (options.autopilot) {
    if (!options.tick_owner) {
      throw std::invalid_argument(
          "--autopilot requires this runtime to own simulation ticks");
    }
    traffic_manager.emplace(client.GetInstanceTM());
    traffic_manager_guard.emplace(*traffic_manager, vehicle);
    std::cout << "Traffic Manager autopilot: enabled (synchronous)\n";
  }

  carla::SharedPtr<cc::Actor> spectator;
  if (options.chase_camera) {
    spectator = world.GetSpectator();
    if (!spectator) {
      throw std::runtime_error("CARLA returned no spectator actor");
    }
    spectator->SetTransform(ChaseCameraTransform(*vehicle));
    std::cout << "Chase camera: enabled\n";
  }

  ConfigureStopSignals();
  const auto started_at = std::chrono::steady_clock::now();
  const auto run_id = GenerateRunId();
  std::optional<SimulationClockAnchor> clock_anchor;
  LatestGnssFixStore gnss_store;
  auto gnss_sensor = SpawnGnssSensor(
      world, *vehicle, options.gnss_sensor_tick_seconds);
  OwnedSensorGuard gnss_guard(gnss_sensor);
  gnss_sensor->Listen([&gnss_store](
                          carla::SharedPtr<carla::sensor::SensorData> data) {
    const auto measurement =
        std::dynamic_pointer_cast<carla::sensor::data::GnssMeasurement>(data);
    if (!measurement) {
      return;
    }
    gnss_store.Publish({
        static_cast<std::uint64_t>(measurement->GetFrame()),
        measurement->GetTimestamp(),
        measurement->GetLatitude(),
        measurement->GetLongitude(),
        measurement->GetAltitude()});
  });
  std::cout << "GNSS sensor: enabled (period "
            << options.gnss_sensor_tick_seconds << " s, actor id="
            << gnss_sensor->GetId() << ")\n";
  LatestVssSignalStore signal_store;
  std::uint64_t frame_count = 0;
  auto next_tick_at = started_at;

  while (!ReachedStopCondition(options, frame_count, started_at)) {
    if (options.tick_owner && options.real_time) {
      next_tick_at += std::chrono::duration_cast<
          std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(options.fixed_delta_seconds));
      std::this_thread::sleep_until(next_tick_at);
    }
    cc::WorldSnapshot snapshot = options.tick_owner
                                     ? (world.Tick(timeout), world.GetSnapshot())
                                     : world.WaitForTick(timeout);
    const double simulation_time_s = snapshot.GetTimestamp().elapsed_seconds;
    if (!clock_anchor.has_value()) {
      clock_anchor.emplace(simulation_time_s,
                           std::chrono::system_clock::now());
    }

    const auto normalized = NormalizeVehicleSample(
        CollectSample(snapshot, *vehicle, run_id, *clock_anchor));
    std::optional<NormalizedGnssFix> normalized_gnss;
    const auto gnss_sample = gnss_store.LatestFor(
        normalized.frame_id, normalized.simulation_time_s,
        options.gnss_max_age_seconds);
    if (gnss_sample.has_value()) {
      normalized_gnss = NormalizeGnssSample(*gnss_sample, *clock_anchor);
    }
    auto vss_snapshot = ProjectToVss(normalized, normalized_gnss);
    if (!signal_store.Publish(std::move(vss_snapshot))) {
      throw std::runtime_error("duplicate or out-of-order CARLA frame " +
                               std::to_string(snapshot.GetFrame()));
    }
    ++frame_count;

    if (spectator) {
      spectator->SetTransform(ChaseCameraTransform(*vehicle));
    }

    if (frame_count == 1 ||
        frame_count % options.log_every_frames == 0) {
      std::cout << "VSS frame=" << normalized.frame_id
                << " simulation_time=" << normalized.simulation_time_s
                << " timestamp=" << signal_store.Latest()->timestamp
                << " speed_kmh=" << normalized.speed_mps * 3.6
                << " gnss_frame="
                << (normalized_gnss.has_value()
                        ? std::to_string(normalized_gnss->source_frame_id)
                        : std::string("unavailable"))
                << " points=" << signal_store.Latest()->data_points.size()
                << '\n';
    }
  }

  std::cout << "Published " << signal_store.publish_count()
            << " frame-aligned VSS state update(s); retained snapshots=1\n"
            << "GNSS fixes accepted=" << gnss_store.publish_count()
            << " rejected=" << gnss_store.rejected_count() << '\n';
  if (!gnss_guard.Destroy()) {
    throw std::runtime_error("failed to destroy GNSS sensor");
  }
  if (traffic_manager_guard) {
    traffic_manager_guard->Restore();
  }
  settings_guard.Restore();
}

}  // namespace

int RunRuntime(const RuntimeOptions &options) {
  try {
    std::cout << "Connecting to CARLA at " << options.host << ':' << options.port
              << " (timeout " << options.timeout_ms << " ms)\n"
              << std::flush;

    cc::Client client(options.host, options.port);
    client.SetTimeout(std::chrono::milliseconds(options.timeout_ms));

    const auto client_version = client.GetClientVersion();
    const auto server_version = client.GetServerVersion();
    std::cout << "LibCarla client version: " << client_version << '\n'
              << "CARLA server version:   " << server_version << '\n';

    if (client_version != server_version) {
      const auto message = "LibCarla/server version mismatch: " + client_version +
                           " vs " + server_version;
      if (options.require_matching_versions) {
        throw std::runtime_error(message);
      }
      std::cerr << "Warning: " << message << '\n';
    }

    auto world = client.GetWorld();
    const auto map = world.GetMap();
    if (!map) {
      throw std::runtime_error("CARLA returned no current map");
    }
    std::cout << "Current map: " << map->GetName() << '\n' << std::flush;

    auto ego_vehicle = FindEgoVehicle(world, options.role_name);
    std::optional<OwnedActorGuard> owned_actor;
    if (ego_vehicle) {
      std::cout << "Selected existing ego vehicle";
    } else {
      if (!options.spawn_if_missing) {
        throw std::runtime_error("no vehicle with role_name='" +
                                 options.role_name + "' was found");
      }
      ego_vehicle = SpawnEgoVehicle(world, options);
      owned_actor.emplace(ego_vehicle);
      std::cout << "Spawned runtime-owned ego vehicle";
    }

    std::cout << " id=" << ego_vehicle->GetId()
              << " type=" << ego_vehicle->GetTypeId()
              << " role_name=" << options.role_name << '\n';

    CollectVehicleState(client, world, ego_vehicle, options);

    if (owned_actor && !owned_actor->Destroy()) {
      return 4;
    }
    return 0;
  } catch (const cc::TimeoutException &error) {
    std::cerr << "CARLA connection timed out: " << error.what() << '\n';
    return 3;
  } catch (const std::exception &error) {
    std::cerr << "CARLA runtime error: " << error.what() << '\n';
    return 4;
  }
}

}  // namespace carla_ego_runtime
