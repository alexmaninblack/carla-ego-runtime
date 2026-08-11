#include "carla_ego_runtime/runtime.hpp"

#include <carla/client/Actor.h>
#include <carla/client/ActorAttribute.h>
#include <carla/client/ActorBlueprint.h>
#include <carla/client/ActorList.h>
#include <carla/client/BlueprintLibrary.h>
#include <carla/client/Client.h>
#include <carla/client/Map.h>
#include <carla/client/TimeoutException.h>
#include <carla/client/Vehicle.h>
#include <carla/client/World.h>

#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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

void WaitForRequestedDuration(std::uint32_t run_seconds) {
  if (run_seconds == 0) {
    return;
  }

  stop_requested = 0;
  std::signal(SIGINT, RequestStop);
  std::signal(SIGTERM, RequestStop);

  std::cout << "Keeping the runtime alive for " << run_seconds
            << " seconds; press Ctrl-C to stop early.\n";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds);
  while (stop_requested == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
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

    WaitForRequestedDuration(options.run_seconds);

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
