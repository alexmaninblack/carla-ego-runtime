#!/usr/bin/env python3
"""Deterministic, replaceable CARLA BehaviorAgent control source for M5."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


ALLOWED_BEHAVIORS = {"cautious", "normal", "aggressive"}
STOP_REQUESTED = False


class ConfigurationError(ValueError):
    """Raised when the M5 route configuration is unsafe or ambiguous."""


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def emit(event: str, **fields: Any) -> None:
    record = {"ts": utc_now(), "source": "behavior_agent", "event": event}
    record.update(fields)
    print(json.dumps(record, sort_keys=True, separators=(",", ":")), flush=True)


def atomic_write_json(path: Optional[Path], payload: Dict[str, Any]) -> None:
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def _require_object(config: Dict[str, Any], key: str) -> Dict[str, Any]:
    value = config.get(key)
    if not isinstance(value, dict):
        raise ConfigurationError(f"{key} must be an object")
    return value


def _require_string(section: Dict[str, Any], key: str) -> str:
    value = section.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ConfigurationError(f"{key} must be a non-empty string")
    return value


def _require_integer(
    section: Dict[str, Any], key: str, minimum: int, maximum: int
) -> int:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigurationError(f"{key} must be an integer")
    if value < minimum or value > maximum:
        raise ConfigurationError(f"{key} must be between {minimum} and {maximum}")
    return value


def _require_number(
    section: Dict[str, Any], key: str, minimum: float, maximum: float
) -> float:
    value = section.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigurationError(f"{key} must be a number")
    value = float(value)
    if not math.isfinite(value) or value < minimum or value > maximum:
        raise ConfigurationError(f"{key} must be between {minimum} and {maximum}")
    return value


def validate_config(config: Dict[str, Any]) -> Dict[str, Any]:
    if config.get("schema_version") != 1:
        raise ConfigurationError("schema_version must be 1")

    controller = _require_object(config, "controller")
    if _require_string(controller, "type") != "behavior_agent":
        raise ConfigurationError("controller.type must be behavior_agent")
    behavior = _require_string(controller, "behavior")
    if behavior not in ALLOWED_BEHAVIORS:
        raise ConfigurationError(
            "controller.behavior must be cautious, normal, or aggressive"
        )

    carla_config = _require_object(config, "carla")
    _require_string(carla_config, "host")
    _require_integer(carla_config, "port", 1, 65535)
    _require_number(carla_config, "timeout_seconds", 1, 300)
    _require_string(carla_config, "expected_map")
    commit = _require_string(carla_config, "source_commit")
    if len(commit) != 40 or any(character not in "0123456789abcdef" for character in commit):
        raise ConfigurationError("carla.source_commit must be a 40-character SHA-1")

    simulation = _require_object(config, "simulation")
    _require_number(simulation, "fixed_delta_seconds", 0.01, 0.2)
    if not isinstance(simulation.get("real_time"), bool):
        raise ConfigurationError("simulation.real_time must be a boolean")
    _require_number(simulation, "maximum_route_seconds", 10, 86400)
    _require_number(simulation, "startup_gate_timeout_seconds", 5, 600)
    _require_number(simulation, "shutdown_gate_timeout_seconds", 1, 600)

    vehicle = _require_object(config, "vehicle")
    _require_string(vehicle, "blueprint")
    _require_string(vehicle, "role_name")

    route = _require_object(config, "route")
    _require_string(route, "name")
    _require_integer(route, "start_spawn_point", 0, 100000)
    destinations = route.get("destination_spawn_points")
    if not isinstance(destinations, list) or not destinations:
        raise ConfigurationError("route.destination_spawn_points must be non-empty")
    for destination in destinations:
        if isinstance(destination, bool) or not isinstance(destination, int) or destination < 0:
            raise ConfigurationError(
                "route.destination_spawn_points must contain non-negative integers"
            )
    _require_integer(route, "cycles", 1, 100)
    _require_number(route, "minimum_leg_distance_m", 1, 100000)

    runtime = _require_object(config, "runtime")
    _require_string(runtime, "viss_bind_address")
    _require_integer(runtime, "viss_port", 1, 65535)
    _require_integer(runtime, "log_every_frames", 1, 1000000)
    if not isinstance(runtime.get("chase_camera"), bool):
        raise ConfigurationError("runtime.chase_camera must be a boolean")
    _require_number(runtime, "chase_camera_response", 0.1, 100)
    _require_integer(runtime, "chase_camera_update_hz", 20, 240)
    _require_number(runtime, "exposure_offset", -5, 5)
    return config


def load_config(path: Path) -> Dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"cannot read {path}: {error}") from error
    if not isinstance(payload, dict):
        raise ConfigurationError("configuration root must be an object")
    return validate_config(payload)


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def has_role_name(actor: Any, role_name: str) -> bool:
    return actor.attributes.get("role_name") == role_name


def route_distance(route_trace: Iterable[Tuple[Any, Any]]) -> float:
    waypoints = [waypoint for waypoint, _road_option in route_trace]
    return sum(
        first.transform.location.distance(second.transform.location)
        for first, second in zip(waypoints, waypoints[1:])
    )


def pace_tick(world: Any, timeout: float, next_tick_at: float, period: float) -> float:
    if period > 0:
        next_tick_at += period
        remaining = next_tick_at - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
    world.tick(timeout)
    return next_tick_at


def hold_synchronous_world(
    world: Any, timeout: float, period: float, stop_event: Any
) -> None:
    """Keep actors published while the observer runtime starts and stops."""
    next_tick_at = time.monotonic()
    try:
        while not stop_event.is_set() and not STOP_REQUESTED:
            next_tick_at = pace_tick(world, timeout, next_tick_at, period)
    except RuntimeError as error:
        emit("world_hold_failed", error=str(error))


def wait_for_file(path: Optional[Path], timeout: float, event: str) -> None:
    if path is None:
        return
    deadline = time.monotonic() + timeout
    while not path.exists():
        if STOP_REQUESTED:
            raise InterruptedError("stop requested")
        if time.monotonic() >= deadline:
            raise TimeoutError(f"timed out waiting for {path}")
        time.sleep(0.05)
    emit(event, path=str(path))


def import_carla(python_api_root: Path) -> Tuple[Any, Any]:
    if not python_api_root.is_dir():
        raise RuntimeError(f"CARLA Python API root is not a directory: {python_api_root}")
    sys.path.insert(0, str(python_api_root))
    try:
        import carla  # type: ignore
        from agents.navigation.behavior_agent import BehaviorAgent  # type: ignore
    except ImportError as error:
        raise RuntimeError(
            "CARLA Python wheel and its agents package are required; see the M5 runbook"
        ) from error
    return carla, BehaviorAgent


def run_controller(
    config: Dict[str, Any], python_api_root: Path, status_file: Optional[Path],
    gate_file: Optional[Path], stop_file: Optional[Path]
) -> int:
    carla, behavior_agent_class = import_carla(python_api_root)
    carla_config = config["carla"]
    simulation = config["simulation"]
    vehicle_config = config["vehicle"]
    route_config = config["route"]
    controller_config = config["controller"]

    status: Dict[str, Any] = {
        "schema_version": 1,
        "state": "starting",
        "updated_at": utc_now(),
        "route": route_config["name"],
    }
    atomic_write_json(status_file, status)

    client = carla.Client(carla_config["host"], carla_config["port"])
    client.set_timeout(float(carla_config["timeout_seconds"]))
    client_version = client.get_client_version()
    server_version = client.get_server_version()
    if client_version != server_version:
        raise RuntimeError(
            f"CARLA client/server version mismatch: {client_version} vs {server_version}"
        )
    world = client.get_world()
    carla_map = world.get_map()
    if carla_map.name != carla_config["expected_map"]:
        raise RuntimeError(
            f"expected map {carla_config['expected_map']}, got {carla_map.name}"
        )

    original_settings = world.get_settings()
    vehicle = None
    settings_changed = False
    completed = False
    try:
        existing = [
            actor
            for actor in world.get_actors().filter("vehicle.*")
            if has_role_name(actor, vehicle_config["role_name"])
        ]
        if existing:
            raise RuntimeError(
                f"an existing vehicle already has role_name={vehicle_config['role_name']}"
            )

        settings = world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = float(simulation["fixed_delta_seconds"])
        world.apply_settings(settings)
        settings_changed = True

        spawn_points = carla_map.get_spawn_points()
        start_index = route_config["start_spawn_point"]
        all_indices: List[int] = [start_index] + list(
            route_config["destination_spawn_points"]
        )
        if any(index >= len(spawn_points) for index in all_indices):
            raise RuntimeError(
                f"route references a spawn point outside 0..{len(spawn_points) - 1}"
            )

        blueprint = world.get_blueprint_library().find(vehicle_config["blueprint"])
        if blueprint is None:
            raise RuntimeError(f"vehicle blueprint is unavailable: {vehicle_config['blueprint']}")
        if not blueprint.has_attribute("role_name"):
            raise RuntimeError("vehicle blueprint has no role_name attribute")
        blueprint.set_attribute("role_name", vehicle_config["role_name"])
        vehicle = world.try_spawn_actor(blueprint, spawn_points[start_index])
        if vehicle is None:
            raise RuntimeError(f"start spawn point {start_index} is occupied")
        # Publish the newly spawned actor into a synchronous world snapshot
        # before the observer runtime is allowed to discover it.
        world.tick(float(carla_config["timeout_seconds"]))

        agent = behavior_agent_class(
            vehicle, behavior=controller_config["behavior"], map_inst=carla_map
        )
        destination_indices = list(route_config["destination_spawn_points"]) * int(
            route_config["cycles"]
        )
        legs = []
        origin = spawn_points[start_index].location
        for destination_index in destination_indices:
            destination = spawn_points[destination_index].location
            trace = agent.trace_route(
                carla_map.get_waypoint(origin), carla_map.get_waypoint(destination)
            )
            distance = route_distance(trace)
            if distance < float(route_config["minimum_leg_distance_m"]):
                raise RuntimeError(
                    f"route leg to {destination_index} is only {distance:.1f} m"
                )
            legs.append({"destination_spawn_point": destination_index, "distance_m": distance})
            origin = destination

        status.update(
            {
                "state": "ready",
                "updated_at": utc_now(),
                "map": carla_map.name,
                "client_version": client_version,
                "server_version": server_version,
                "vehicle_id": vehicle.id,
                "vehicle_type": vehicle.type_id,
                "start_spawn_point": start_index,
                "legs": legs,
                "total_distance_m": sum(leg["distance_m"] for leg in legs),
            }
        )
        atomic_write_json(status_file, status)
        emit(
            "controller_ready",
            map=carla_map.name,
            vehicle_id=vehicle.id,
            route=route_config["name"],
            total_distance_m=round(status["total_distance_m"], 1),
        )
        if gate_file is not None:
            gate_stop = threading.Event()
            gate_thread = threading.Thread(
                target=hold_synchronous_world,
                args=(
                    world,
                    float(carla_config["timeout_seconds"]),
                    float(simulation["fixed_delta_seconds"])
                    if simulation["real_time"]
                    else 0.0,
                    gate_stop,
                ),
            )
            gate_thread.start()
            try:
                wait_for_file(
                    gate_file,
                    float(simulation["startup_gate_timeout_seconds"]),
                    "startup_gate_opened",
                )
            finally:
                gate_stop.set()
                gate_thread.join()

        route_started_at = time.monotonic()
        next_tick_at = route_started_at
        period = (
            float(simulation["fixed_delta_seconds"])
            if simulation["real_time"]
            else 0.0
        )
        frame_count = 0
        for leg_number, destination_index in enumerate(destination_indices, start=1):
            destination = spawn_points[destination_index].location
            agent.set_destination(destination, start_location=vehicle.get_location())
            emit(
                "route_leg_started",
                leg=leg_number,
                destination_spawn_point=destination_index,
            )
            while not agent.done():
                if STOP_REQUESTED:
                    raise InterruptedError("stop requested")
                if (
                    time.monotonic() - route_started_at
                    > float(simulation["maximum_route_seconds"])
                ):
                    raise TimeoutError("maximum route duration exceeded")
                vehicle.apply_control(agent.run_step())
                next_tick_at = pace_tick(
                    world,
                    float(carla_config["timeout_seconds"]),
                    next_tick_at,
                    period,
                )
                frame_count += 1
            emit("route_leg_completed", leg=leg_number, frames=frame_count)

        completed = True
        vehicle.apply_control(carla.VehicleControl(throttle=0.0, brake=1.0))
        status.update(
            {
                "state": "route_complete",
                "updated_at": utc_now(),
                "frames": frame_count,
                "elapsed_seconds": time.monotonic() - route_started_at,
            }
        )
        atomic_write_json(status_file, status)
        emit(
            "route_completed",
            frames=frame_count,
            elapsed_seconds=round(status["elapsed_seconds"], 3),
        )

        if stop_file is not None:
            shutdown_stop = threading.Event()
            shutdown_thread = threading.Thread(
                target=hold_synchronous_world,
                args=(
                    world,
                    float(carla_config["timeout_seconds"]),
                    period,
                    shutdown_stop,
                ),
            )
            shutdown_thread.start()
            try:
                wait_for_file(
                    stop_file,
                    float(simulation["shutdown_gate_timeout_seconds"]),
                    "shutdown_gate_closed",
                )
            finally:
                shutdown_stop.set()
                shutdown_thread.join()
        return 0
    finally:
        if vehicle is not None:
            try:
                vehicle.apply_control(carla.VehicleControl(throttle=0.0, brake=1.0))
                vehicle.destroy()
                emit("vehicle_destroyed", vehicle_id=vehicle.id)
            except RuntimeError as error:
                emit("vehicle_cleanup_failed", error=str(error))
        if settings_changed:
            try:
                world.apply_settings(original_settings)
                emit("world_settings_restored")
            except RuntimeError as error:
                emit("world_settings_restore_failed", error=str(error))
        status.update(
            {
                "state": "completed" if completed else "failed",
                "updated_at": utc_now(),
            }
        )
        atomic_write_json(status_file, status)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--python-api-root", type=Path)
    parser.add_argument("--status-file", type=Path)
    parser.add_argument("--gate-file", type=Path)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        config = load_config(arguments.config)
        if arguments.validate_only:
            emit("configuration_valid", config=str(arguments.config))
            return 0
        if arguments.python_api_root is None:
            raise ConfigurationError("--python-api-root is required for a live run")
        signal.signal(signal.SIGINT, request_stop)
        signal.signal(signal.SIGTERM, request_stop)
        return run_controller(
            config,
            arguments.python_api_root,
            arguments.status_file,
            arguments.gate_file,
            arguments.stop_file,
        )
    except (ConfigurationError, RuntimeError, TimeoutError, InterruptedError) as error:
        emit("controller_failed", error=str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
