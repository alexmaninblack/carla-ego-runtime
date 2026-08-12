#!/usr/bin/env python3
"""M6 CARLA tick owner controlled through the authenticated local contract."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import math
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, Optional


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


TOOLS = Path(__file__).resolve().parent
M5 = load_module("m6_m5_helpers", TOOLS / "behavior_agent_controller.py")
PROTOCOL = load_module("m6_control_protocol", TOOLS / "external_control_protocol.py")
STOP_REQUESTED = False


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def emit(event: str, **fields: Any) -> None:
    record = {
        "ts": utc_now(),
        "source": "external_controller",
        "event": event,
        **fields,
    }
    print(json.dumps(record, sort_keys=True, separators=(",", ":")), flush=True)


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


def atomic_write_json(path: Optional[Path], payload: Dict[str, Any]) -> None:
    if path is None:
        return
    M5.atomic_write_json(path, payload)


def run_controller(arguments: argparse.Namespace, config: Dict[str, Any]) -> int:
    carla, _ = M5.import_carla(arguments.python_api_root)
    carla_config = config["carla"]
    simulation = config["simulation"]
    vehicle_config = config["vehicle"]
    route_config = config["route"]
    control_config = config["controller"]["external_control"]
    status: Dict[str, Any] = {
        "schema_version": 1,
        "state": "starting",
        "updated_at": utc_now(),
        "control_source": "external_control",
    }
    atomic_write_json(arguments.status_file, status)

    client = carla.Client(carla_config["host"], carla_config["port"])
    client.set_timeout(float(carla_config["timeout_seconds"]))
    if client.get_client_version() != client.get_server_version():
        raise RuntimeError("CARLA client/server version mismatch")
    world = client.get_world()
    carla_map = world.get_map()
    if carla_map.name != carla_config["expected_map"]:
        raise RuntimeError(
            f"expected map {carla_config['expected_map']}, got {carla_map.name}"
        )

    original_settings = world.get_settings()
    vehicle = None
    settings_changed = False
    server = None
    completed = False
    stopped = False
    control_events = []
    status_lock = threading.Lock()

    def write_status(**fields: Any) -> None:
        with status_lock:
            status.update(fields)
            status["updated_at"] = utc_now()
            atomic_write_json(arguments.status_file, status)

    def emit_control(event: str, fields: Dict[str, Any]) -> None:
        control_events.append({"ts": utc_now(), "event": event, **fields})
        emit(event, **fields)

    def write_control_snapshot(snapshot: Dict[str, Any]) -> None:
        write_status(control=snapshot)

    try:
        existing = [
            actor
            for actor in world.get_actors().filter("vehicle.*")
            if M5.has_role_name(actor, vehicle_config["role_name"])
        ]
        if existing:
            raise RuntimeError("an existing hero vehicle already exists")

        settings = world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = float(simulation["fixed_delta_seconds"])
        world.apply_settings(settings)
        settings_changed = True

        spawn_points = carla_map.get_spawn_points()
        start_index = int(route_config["start_spawn_point"])
        if start_index >= len(spawn_points):
            raise RuntimeError("external-control spawn point is unavailable")
        blueprint = world.get_blueprint_library().find(vehicle_config["blueprint"])
        if blueprint is None or not blueprint.has_attribute("role_name"):
            raise RuntimeError("configured vehicle blueprint is unavailable")
        blueprint.set_attribute("role_name", vehicle_config["role_name"])
        vehicle = world.try_spawn_actor(blueprint, spawn_points[start_index])
        if vehicle is None:
            raise RuntimeError(f"start spawn point {start_index} is occupied")
        world.tick(float(carla_config["timeout_seconds"]))

        token = PROTOCOL.LocalControlServer.create_token_file(arguments.token_file)
        control_state = PROTOCOL.ExternalControlState(
            token,
            float(control_config["command_timeout_seconds"]),
            float(control_config["ownership_timeout_seconds"]),
            emit_control,
        )
        server = PROTOCOL.LocalControlServer(
            arguments.socket_file,
            arguments.token_file,
            control_state,
            write_control_snapshot,
        )
        server.start()
        write_status(
            state="ready",
            map=carla_map.name,
            vehicle_id=vehicle.id,
            vehicle_type=vehicle.type_id,
            socket=arguments.socket_file.name,
            token=arguments.token_file.name,
        )
        emit(
            "external_controller_ready",
            vehicle_id=vehicle.id,
            map=carla_map.name,
        )

        if arguments.gate_file is not None:
            gate_stop = threading.Event()
            gate_thread = threading.Thread(
                target=M5.hold_synchronous_world,
                args=(
                    world,
                    float(carla_config["timeout_seconds"]),
                    float(simulation["fixed_delta_seconds"]),
                    gate_stop,
                ),
            )
            gate_thread.start()
            try:
                wait_for_file(
                    arguments.gate_file,
                    float(simulation["startup_gate_timeout_seconds"]),
                    "startup_gate_opened",
                )
            finally:
                gate_stop.set()
                gate_thread.join()

        started_at = time.monotonic()
        next_tick_at = started_at
        period = float(simulation["fixed_delta_seconds"])
        frame_count = 0
        last_location = vehicle.get_location()
        total_distance_m = 0.0
        maximum_speed_kmh = 0.0
        current_speed_kmh = 0.0
        last_motion_status_at = started_at
        last_safe_stop: Optional[bool] = None
        last_reason = ""
        while not STOP_REQUESTED:
            now = time.monotonic()
            if now - started_at > float(control_config["maximum_session_seconds"]):
                completed = True
                break
            applied = control_state.current_control(now)
            vehicle.apply_control(
                carla.VehicleControl(
                    throttle=applied.throttle,
                    brake=applied.brake,
                    steer=applied.steering,
                )
            )
            if applied.safe_stop != last_safe_stop or applied.reason != last_reason:
                emit(
                    "control_applied",
                    sequence=applied.sequence,
                    safe_stop=applied.safe_stop,
                    reason=applied.reason,
                )
                last_safe_stop = applied.safe_stop
                last_reason = applied.reason
                write_control_snapshot(control_state.snapshot())
            next_tick_at = M5.pace_tick(
                world,
                float(carla_config["timeout_seconds"]),
                next_tick_at,
                period,
            )
            frame_count += 1
            location = vehicle.get_location()
            total_distance_m += math.sqrt(
                (location.x - last_location.x) ** 2
                + (location.y - last_location.y) ** 2
                + (location.z - last_location.z) ** 2
            )
            last_location = location
            velocity = vehicle.get_velocity()
            current_speed_kmh = 3.6 * math.sqrt(
                velocity.x**2 + velocity.y**2 + velocity.z**2
            )
            maximum_speed_kmh = max(maximum_speed_kmh, current_speed_kmh)
            if now - last_motion_status_at >= 1.0:
                write_status(
                    motion={
                        "total_distance_m": total_distance_m,
                        "maximum_speed_kmh": maximum_speed_kmh,
                        "current_speed_kmh": current_speed_kmh,
                    }
                )
                last_motion_status_at = now

        stopped = STOP_REQUESTED
        vehicle.apply_control(carla.VehicleControl(throttle=0.0, brake=1.0, steer=0.0))
        write_status(
            state="session_complete" if completed else "stopped",
            frames=frame_count,
            elapsed_seconds=time.monotonic() - started_at,
            control=control_state.snapshot(),
            motion={
                "total_distance_m": total_distance_m,
                "maximum_speed_kmh": maximum_speed_kmh,
                "current_speed_kmh": current_speed_kmh,
            },
        )

        if arguments.stop_file is not None and completed:
            shutdown_stop = threading.Event()
            shutdown_thread = threading.Thread(
                target=M5.hold_synchronous_world,
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
                    arguments.stop_file,
                    float(simulation["shutdown_gate_timeout_seconds"]),
                    "shutdown_gate_closed",
                )
            finally:
                shutdown_stop.set()
                shutdown_thread.join()
        return 0
    finally:
        if server is not None:
            server.stop()
        if vehicle is not None:
            try:
                vehicle.apply_control(
                    carla.VehicleControl(throttle=0.0, brake=1.0, steer=0.0)
                )
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
        write_status(
            state=(
                "completed" if completed else "stopped" if stopped else "failed"
            ),
            control_events=control_events,
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--status-file", required=True, type=Path)
    parser.add_argument("--gate-file", type=Path)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--socket-file", required=True, type=Path)
    parser.add_argument("--token-file", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        config = M5.load_config(arguments.config)
        if config["controller"]["type"] != "external_control":
            raise M5.ConfigurationError(
                "controller.type must be external_control for M6"
            )
        return run_controller(arguments, config)
    except (
        M5.ConfigurationError,
        RuntimeError,
        TimeoutError,
        OSError,
        InterruptedError,
    ) as error:
        emit("external_controller_failed", error=str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
