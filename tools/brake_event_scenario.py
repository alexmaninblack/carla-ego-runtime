#!/usr/bin/env python3
"""Run one deterministic stationary-obstacle braking scenario in CARLA."""

# Copyright (c) 2026 maninblack
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import signal
import sys
import threading
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


TOOLS = Path(__file__).resolve().parent
M5 = load_module("brake_scenario_m5_helpers", TOOLS / "behavior_agent_controller.py")
STOP_REQUESTED = False


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def emit(event: str, **fields: Any) -> None:
    record = {
        "ts": M5.utc_now(),
        "source": "brake_event_scenario",
        "event": event,
        **fields,
    }
    print(json.dumps(record, sort_keys=True, separators=(",", ":")), flush=True)


def import_carla(python_api_root: Path) -> Any:
    if not python_api_root.is_dir():
        raise RuntimeError(
            f"CARLA Python API root is not a directory: {python_api_root}"
        )
    sys.path.insert(0, str(python_api_root))
    try:
        import carla  # type: ignore
    except ImportError as error:
        raise RuntimeError("the CARLA Python wheel is required") from error
    return carla


def speed_kmh(vehicle: Any) -> float:
    velocity = vehicle.get_velocity()
    return 3.6 * math.sqrt(velocity.x**2 + velocity.y**2 + velocity.z**2)


def waypoint_sort_key(current: Any, candidate: Any) -> Tuple[float, int, int, float]:
    current_forward = current.transform.get_forward_vector()
    candidate_forward = candidate.transform.get_forward_vector()
    alignment = (
        current_forward.x * candidate_forward.x
        + current_forward.y * candidate_forward.y
        + current_forward.z * candidate_forward.z
    )
    return (
        -alignment,
        int(getattr(candidate, "road_id", 0)),
        int(getattr(candidate, "lane_id", 0)),
        float(getattr(candidate, "s", 0.0)),
    )


def choose_forward_waypoint(current: Any, distance_m: float) -> Any:
    candidates = list(current.next(distance_m))
    if not candidates:
        raise RuntimeError(
            f"the driving lane ends before a {distance_m:.1f} m lookahead"
        )
    return min(candidates, key=lambda item: waypoint_sort_key(current, item))


def advance_waypoint(start: Any, distance_m: float, step_m: float = 2.0) -> Any:
    current = start
    travelled = 0.0
    while travelled + 1.0e-9 < distance_m:
        step = min(step_m, distance_m - travelled)
        current = choose_forward_waypoint(current, step)
        travelled += step
    return current


def steering_command(vehicle_transform: Any, target_location: Any, maximum: float) -> float:
    delta_x = target_location.x - vehicle_transform.location.x
    delta_y = target_location.y - vehicle_transform.location.y
    forward = vehicle_transform.get_forward_vector()
    right = vehicle_transform.get_right_vector()
    forward_component = delta_x * forward.x + delta_y * forward.y
    right_component = delta_x * right.x + delta_y * right.y
    heading_error = math.atan2(right_component, max(0.01, forward_component))
    return max(-maximum, min(maximum, heading_error / math.radians(45.0)))


def obstacle_gap_m(ego: Any, obstacle: Any) -> float:
    center_distance = ego.get_location().distance(obstacle.get_location())
    return center_distance - ego.bounding_box.extent.x - obstacle.bounding_box.extent.x


def longitudinal_acceleration_mps2(vehicle: Any) -> float:
    acceleration = vehicle.get_acceleration()
    forward = vehicle.get_transform().get_forward_vector()
    return (
        acceleration.x * forward.x
        + acceleration.y * forward.y
        + acceleration.z * forward.z
    )


@dataclass(frozen=True)
class ScenarioControl:
    throttle: float
    brake: float
    phase: str
    completed: bool


class BrakeScenarioStateMachine:
    def __init__(self, config: Dict[str, Any], fixed_delta_seconds: float):
        self.config = config
        self.fixed_delta_seconds = fixed_delta_seconds
        self.phase = "ACCELERATE"
        self.phase_frames = 0
        self.stopped_frames = 0
        self.brake_onset_speed_kmh: Optional[float] = None

    def _speed_control(self, current_speed_kmh: float) -> Tuple[float, float]:
        target = float(self.config["target_speed_kmh"])
        tolerance = float(self.config["target_speed_tolerance_kmh"])
        error = target - current_speed_kmh
        if error < -tolerance:
            return 0.0, min(0.15, abs(error) * 0.02)
        throttle = 0.16 + float(self.config["speed_control_gain"]) * error
        return max(0.0, min(float(self.config["acceleration_throttle"]), throttle)), 0.0

    def step(self, current_speed_kmh: float, gap_m: float) -> ScenarioControl:
        previous_phase = self.phase
        target = float(self.config["target_speed_kmh"])
        tolerance = float(self.config["target_speed_tolerance_kmh"])

        if self.phase not in {"BRAKE", "HOLD", "COMPLETE"} and gap_m <= float(
            self.config["brake_trigger_gap_m"]
        ):
            self.phase = "BRAKE"
            self.phase_frames = 0
            self.brake_onset_speed_kmh = current_speed_kmh
        elif self.phase == "ACCELERATE" and current_speed_kmh >= target - tolerance:
            self.phase = "STABILIZE"
            self.phase_frames = 0
        elif self.phase == "STABILIZE":
            required = math.ceil(
                float(self.config["stabilization_seconds"])
                / self.fixed_delta_seconds
            )
            if self.phase_frames >= required:
                self.phase = "APPROACH"
                self.phase_frames = 0
        elif self.phase == "BRAKE":
            if current_speed_kmh <= float(self.config["stopped_speed_kmh"]):
                self.stopped_frames += 1
            else:
                self.stopped_frames = 0
            if self.stopped_frames >= int(self.config["stopped_frames"]):
                self.phase = "HOLD"
                self.phase_frames = 0
        elif self.phase == "HOLD":
            required = math.ceil(
                float(self.config["hold_seconds"]) / self.fixed_delta_seconds
            )
            if self.phase_frames >= required:
                self.phase = "COMPLETE"
                self.phase_frames = 0

        if self.phase != previous_phase:
            emit(
                "scenario_phase_changed",
                previous=previous_phase,
                phase=self.phase,
                speed_kmh=round(current_speed_kmh, 3),
                obstacle_gap_m=round(gap_m, 3),
            )

        self.phase_frames += 1
        if self.phase in {"BRAKE", "HOLD", "COMPLETE"}:
            return ScenarioControl(
                0.0,
                float(self.config["brake_command"]),
                self.phase,
                self.phase == "COMPLETE",
            )
        if self.phase == "ACCELERATE":
            return ScenarioControl(
                float(self.config["acceleration_throttle"]),
                0.0,
                self.phase,
                False,
            )
        throttle, brake = self._speed_control(current_speed_kmh)
        return ScenarioControl(throttle, brake, self.phase, False)


def evaluate_result(
    scenario: Dict[str, Any], metrics: Dict[str, Any]
) -> Tuple[bool, list[str]]:
    reasons = []
    if metrics["collision_count"] != 0:
        reasons.append("a collision was recorded")
    stop_gap = metrics.get("stop_gap_m")
    if stop_gap is None:
        reasons.append("the ego vehicle did not reach a stable stop")
    elif stop_gap < float(scenario["minimum_stop_gap_m"]):
        reasons.append("the final obstacle gap is below the safety minimum")
    elif stop_gap > float(scenario["maximum_stop_gap_m"]):
        reasons.append("the ego vehicle stopped too far from the obstacle")
    onset_speed = metrics.get("brake_onset_speed_kmh")
    if onset_speed is None or abs(
        onset_speed - float(scenario["target_speed_kmh"])
    ) > float(scenario["target_speed_tolerance_kmh"]):
        reasons.append("braking did not start at the configured target speed")
    if abs(float(metrics["peak_deceleration_mps2"])) < float(
        scenario["minimum_peak_deceleration_mps2"]
    ):
        reasons.append("peak deceleration was below the acceptance threshold")
    return not reasons, reasons


def spawn_vehicle(
    world: Any,
    blueprint_library: Any,
    blueprint_id: str,
    role_name: str,
    transform: Any,
) -> Any:
    blueprint = blueprint_library.find(blueprint_id)
    if blueprint is None or not blueprint.has_attribute("role_name"):
        raise RuntimeError(f"vehicle blueprint is unavailable: {blueprint_id}")
    blueprint.set_attribute("role_name", role_name)
    actor = world.try_spawn_actor(blueprint, transform)
    if actor is None:
        raise RuntimeError(f"cannot spawn {role_name} at the configured location")
    return actor


def run_controller(
    arguments: argparse.Namespace, config: Dict[str, Any]
) -> int:
    carla = import_carla(arguments.python_api_root)
    carla_config = config["carla"]
    simulation = config["simulation"]
    vehicle_config = config["vehicle"]
    route = config["route"]
    scenario = config["controller"]["scenario"]
    obstacle_config = scenario["obstacle"]

    status: Dict[str, Any] = {
        "schema_version": 1,
        "state": "starting",
        "updated_at": M5.utc_now(),
        "scenario_id": scenario["id"],
        "phase": "PREFLIGHT",
    }
    M5.atomic_write_json(arguments.status_file, status)

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
    settings_changed = False
    ego = None
    obstacle = None
    collision_sensor = None
    completed = False
    stopped = False
    collision_frames: list[int] = []
    try:
        occupied_roles = {
            actor.attributes.get("role_name")
            for actor in world.get_actors().filter("vehicle.*")
        }
        for role in (vehicle_config["role_name"], obstacle_config["role_name"]):
            if role in occupied_roles:
                raise RuntimeError(f"an existing vehicle already has role_name={role}")

        settings = world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = float(simulation["fixed_delta_seconds"])
        world.apply_settings(settings)
        settings_changed = True

        spawn_points = carla_map.get_spawn_points()
        blueprint_library = world.get_blueprint_library()
        start_index = int(route["start_spawn_point"])
        if start_index >= len(spawn_points):
            raise RuntimeError("the configured ego spawn point is unavailable")
        ego = spawn_vehicle(
            world,
            blueprint_library,
            vehicle_config["blueprint"],
            vehicle_config["role_name"],
            spawn_points[start_index],
        )
        emit("ego_spawned", actor_id=ego.id, spawn_point=start_index)

        emit("obstacle_waypoint_search_started")
        start_waypoint = carla_map.get_waypoint(
            spawn_points[start_index].location,
            project_to_road=True,
            lane_type=carla.LaneType.Driving,
        )
        if start_waypoint is None:
            raise RuntimeError("the ego spawn point is not on a driving lane")
        obstacle_waypoint = advance_waypoint(
            start_waypoint, float(obstacle_config["distance_m"])
        )
        emit(
            "obstacle_waypoint_selected",
            road_id=obstacle_waypoint.road_id,
            lane_id=obstacle_waypoint.lane_id,
            s=round(float(obstacle_waypoint.s), 3),
        )
        obstacle_transform = obstacle_waypoint.transform
        obstacle_transform.location.z += float(obstacle_config["spawn_height_m"])
        obstacle = spawn_vehicle(
            world,
            blueprint_library,
            obstacle_config["blueprint"],
            obstacle_config["role_name"],
            obstacle_transform,
        )
        emit("obstacle_spawned", actor_id=obstacle.id)
        obstacle.apply_control(
            carla.VehicleControl(throttle=0.0, brake=1.0, hand_brake=True)
        )
        obstacle.set_simulate_physics(False)

        collision_blueprint = blueprint_library.find("sensor.other.collision")
        if collision_blueprint is None:
            raise RuntimeError("CARLA collision sensor blueprint is unavailable")
        collision_sensor = world.spawn_actor(
            collision_blueprint, carla.Transform(), attach_to=ego
        )
        collision_sensor.listen(
            lambda event: collision_frames.append(int(event.frame))
        )
        world.tick(float(carla_config["timeout_seconds"]))

        status.update(
            {
                "state": "ready",
                "phase": "SPAWN",
                "updated_at": M5.utc_now(),
                "map": carla_map.name,
                "ego_vehicle_id": ego.id,
                "obstacle_vehicle_id": obstacle.id,
                "start_spawn_point": start_index,
                "obstacle_distance_m": obstacle_config["distance_m"],
            }
        )
        M5.atomic_write_json(arguments.status_file, status)
        emit(
            "scenario_ready",
            scenario_id=scenario["id"],
            ego_vehicle_id=ego.id,
            obstacle_vehicle_id=obstacle.id,
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
                M5.wait_for_file(
                    arguments.gate_file,
                    float(simulation["startup_gate_timeout_seconds"]),
                    "startup_gate_opened",
                )
            finally:
                gate_stop.set()
                gate_thread.join()

        fixed_delta = float(simulation["fixed_delta_seconds"])
        machine = BrakeScenarioStateMachine(scenario, fixed_delta)
        started_at = time.monotonic()
        next_tick_at = started_at
        frame_count = 0
        minimum_gap = math.inf
        peak_deceleration = 0.0
        stop_gap: Optional[float] = None
        last_phase = machine.phase

        while not machine.phase == "COMPLETE":
            if STOP_REQUESTED:
                raise InterruptedError("stop requested")
            if time.monotonic() - started_at > float(
                scenario["maximum_duration_seconds"]
            ):
                raise TimeoutError("braking scenario duration exceeded")

            ego_transform = ego.get_transform()
            current_waypoint = carla_map.get_waypoint(
                ego_transform.location,
                project_to_road=True,
                lane_type=carla.LaneType.Driving,
            )
            if current_waypoint is None:
                raise RuntimeError("ego vehicle left the driving lane")
            target_waypoint = choose_forward_waypoint(
                current_waypoint, float(scenario["lookahead_distance_m"])
            )
            gap = obstacle_gap_m(ego, obstacle)
            current_speed = speed_kmh(ego)
            control = machine.step(current_speed, gap)
            steer = steering_command(
                ego_transform,
                target_waypoint.transform.location,
                float(scenario["maximum_steering"]),
            )
            ego.apply_control(
                carla.VehicleControl(
                    throttle=control.throttle,
                    brake=control.brake,
                    steer=steer,
                )
            )
            next_tick_at = M5.pace_tick(
                world,
                float(carla_config["timeout_seconds"]),
                next_tick_at,
                fixed_delta if simulation["real_time"] else 0.0,
            )
            frame_count += 1
            gap = obstacle_gap_m(ego, obstacle)
            minimum_gap = min(minimum_gap, gap)
            peak_deceleration = min(
                peak_deceleration, longitudinal_acceleration_mps2(ego)
            )
            if machine.phase == "HOLD" and stop_gap is None:
                stop_gap = gap
            if machine.phase != last_phase or frame_count % 15 == 0:
                status.update(
                    {
                        "state": "running",
                        "phase": machine.phase,
                        "updated_at": M5.utc_now(),
                        "frame_count": frame_count,
                        "speed_kmh": round(speed_kmh(ego), 3),
                        "obstacle_gap_m": round(gap, 3),
                        "collision_count": len(collision_frames),
                    }
                )
                M5.atomic_write_json(arguments.status_file, status)
                last_phase = machine.phase

        metrics: Dict[str, Any] = {
            "frames": frame_count,
            "elapsed_seconds": time.monotonic() - started_at,
            "brake_onset_speed_kmh": machine.brake_onset_speed_kmh,
            "peak_deceleration_mps2": peak_deceleration,
            "minimum_obstacle_gap_m": minimum_gap,
            "stop_gap_m": stop_gap,
            "collision_count": len(collision_frames),
            "collision_frames": collision_frames,
        }
        passed, failure_reasons = evaluate_result(scenario, metrics)
        completed = True
        ego.apply_control(
            carla.VehicleControl(throttle=0.0, brake=1.0, steer=0.0)
        )
        status.update(
            {
                "state": "scenario_complete",
                "phase": "EVALUATE",
                "updated_at": M5.utc_now(),
                "result": "PASS" if passed else "FAIL",
                "failure_reasons": failure_reasons,
                "metrics": metrics,
            }
        )
        M5.atomic_write_json(arguments.status_file, status)
        emit(
            "scenario_evaluated",
            result=status["result"],
            failure_reasons=failure_reasons,
            metrics=metrics,
        )

        if arguments.stop_file is not None:
            shutdown_stop = threading.Event()
            shutdown_thread = threading.Thread(
                target=M5.hold_synchronous_world,
                args=(
                    world,
                    float(carla_config["timeout_seconds"]),
                    fixed_delta if simulation["real_time"] else 0.0,
                    shutdown_stop,
                ),
            )
            shutdown_thread.start()
            try:
                M5.wait_for_file(
                    arguments.stop_file,
                    float(simulation["shutdown_gate_timeout_seconds"]),
                    "shutdown_gate_closed",
                )
            finally:
                shutdown_stop.set()
                shutdown_thread.join()
        return 0 if passed else 3
    except InterruptedError:
        stopped = True
        emit("scenario_stopped", reason="operator_request")
        return 0
    finally:
        if collision_sensor is not None:
            try:
                if collision_sensor.is_listening:
                    collision_sensor.stop()
                collision_sensor.destroy()
            except RuntimeError as error:
                emit("collision_sensor_cleanup_failed", error=str(error))
        for name, actor in (("obstacle", obstacle), ("ego", ego)):
            if actor is None:
                continue
            try:
                actor.destroy()
                emit("actor_destroyed", actor=name, actor_id=actor.id)
            except RuntimeError as error:
                emit("actor_cleanup_failed", actor=name, error=str(error))
        if settings_changed:
            try:
                world.apply_settings(original_settings)
                emit("world_settings_restored")
            except RuntimeError as error:
                emit("world_settings_restore_failed", error=str(error))
        status.update(
            {
                "state": (
                    "completed" if completed else "stopped" if stopped else "failed"
                ),
                "phase": "CLEANUP",
                "updated_at": M5.utc_now(),
            }
        )
        M5.atomic_write_json(arguments.status_file, status)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--status-file", required=True, type=Path)
    parser.add_argument("--gate-file", type=Path)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        config = M5.load_config(arguments.config)
        if config["controller"]["type"] != "brake_event_scenario":
            raise M5.ConfigurationError(
                "controller.type must be brake_event_scenario"
            )
        if arguments.validate_only:
            emit("configuration_valid", config=str(arguments.config))
            return 0
        return run_controller(arguments, config)
    except (
        M5.ConfigurationError,
        RuntimeError,
        TimeoutError,
        OSError,
    ) as error:
        emit(
            "scenario_failed",
            error=str(error),
            traceback=traceback.format_exc(),
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
