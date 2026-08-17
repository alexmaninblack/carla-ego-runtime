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
BRAKE = load_module("m6_brake_scenario", TOOLS / "brake_event_scenario.py")
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


def autopilot_unavailable_reason(
    vehicle: Any,
    carla_map: Any,
    carla: Any,
    maximum_road_distance_m: float,
    maximum_heading_error_degrees: float,
) -> Optional[str]:
    transform = vehicle.get_transform()
    waypoint = carla_map.get_waypoint(
        transform.location,
        project_to_road=True,
        lane_type=carla.LaneType.Driving,
    )
    if waypoint is None:
        return "autopilot is unavailable away from a driving lane"
    if transform.location.distance(waypoint.transform.location) > maximum_road_distance_m:
        return "autopilot is unavailable away from the road"
    vehicle_forward = transform.get_forward_vector()
    road_forward = waypoint.transform.get_forward_vector()
    dot = max(
        -1.0,
        min(
            1.0,
            vehicle_forward.x * road_forward.x
            + vehicle_forward.y * road_forward.y
            + vehicle_forward.z * road_forward.z,
        ),
    )
    heading_error = math.degrees(math.acos(dot))
    if heading_error > maximum_heading_error_degrees:
        return "autopilot is unavailable while facing against the driving lane"
    return None


def blend_control(carla: Any, first: Any, second: Any, alpha: float) -> Any:
    alpha = max(0.0, min(1.0, alpha))
    throttle = (1.0 - alpha) * float(first.throttle) + alpha * float(second.throttle)
    brake = (1.0 - alpha) * float(first.brake) + alpha * float(second.brake)
    overlap = min(throttle, brake)
    throttle -= overlap
    brake -= overlap
    return carla.VehicleControl(
        throttle=throttle,
        brake=brake,
        steer=(1.0 - alpha) * float(first.steer) + alpha * float(second.steer),
    )


def reset_scenario_vehicle(carla: Any, vehicle: Any, transform: Any) -> None:
    vehicle.apply_control(
        carla.VehicleControl(throttle=0.0, brake=1.0, steer=0.0)
    )
    vehicle.set_transform(transform)
    vehicle.set_target_velocity(carla.Vector3D(0.0, 0.0, 0.0))
    vehicle.set_target_angular_velocity(carla.Vector3D(0.0, 0.0, 0.0))


def bounded_scenario_metrics(
    frame_count: int,
    elapsed_seconds: float,
    brake_onset_speed_kmh: Optional[float],
    peak_deceleration_mps2: float,
    minimum_obstacle_gap_m: float,
    stop_gap_m: Optional[float],
    collision_frames: list[int],
) -> Dict[str, Any]:
    return {
        "frames": frame_count,
        "elapsed_seconds": elapsed_seconds,
        "brake_onset_speed_kmh": brake_onset_speed_kmh,
        "peak_deceleration_mps2": peak_deceleration_mps2,
        "minimum_obstacle_gap_m": (
            minimum_obstacle_gap_m
            if math.isfinite(minimum_obstacle_gap_m)
            else None
        ),
        "stop_gap_m": stop_gap_m,
        "collision_count": len(collision_frames),
        "collision_frames": list(collision_frames),
    }


def run_controller(arguments: argparse.Namespace, config: Dict[str, Any]) -> int:
    carla, _ = M5.import_carla(arguments.python_api_root)
    carla_config = config["carla"]
    simulation = config["simulation"]
    vehicle_config = config["vehicle"]
    route_config = config["route"]
    controller_config = config["controller"]
    control_config = controller_config["external_control"]
    autopilot_config = controller_config.get("autopilot")
    scenario_config = controller_config.get("scenario")
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
    obstacle = None
    collision_sensor = None
    settings_changed = False
    server = None
    completed = False
    stopped = False
    control_events = []
    scenario_runs: list[Dict[str, Any]] = []
    collision_frames: list[int] = []
    status_lock = threading.Lock()
    availability_lock = threading.Lock()
    autopilot_unavailable = (
        "autopilot is not configured" if autopilot_config is None else None
    )
    available_modes = {"safe_stop", "manual"}
    if autopilot_config is not None:
        available_modes.add("autopilot")
    if scenario_config is not None:
        available_modes.add("scenario")

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

    def validate_mode(mode: str) -> Optional[str]:
        if mode == "scenario" and scenario_config is None:
            return "scripted scenario is not configured"
        if mode != "autopilot":
            return None
        with availability_lock:
            return autopilot_unavailable

    try:
        occupied_roles = {
            actor.attributes.get("role_name")
            for actor in world.get_actors().filter("vehicle.*")
        }
        if vehicle_config["role_name"] in occupied_roles:
            raise RuntimeError("an existing hero vehicle already exists")
        if scenario_config is not None:
            obstacle_role = scenario_config["obstacle"]["role_name"]
            if obstacle_role in occupied_roles:
                raise RuntimeError(
                    "an existing brake-event obstacle already exists"
                )

        settings = world.get_settings()
        settings.synchronous_mode = True
        settings.fixed_delta_seconds = float(simulation["fixed_delta_seconds"])
        world.apply_settings(settings)
        settings_changed = True

        spawn_points = carla_map.get_spawn_points()
        start_index = int(route_config["start_spawn_point"])
        if start_index >= len(spawn_points):
            raise RuntimeError("external-control spawn point is unavailable")
        blueprint_library = world.get_blueprint_library()
        blueprint = blueprint_library.find(vehicle_config["blueprint"])
        if blueprint is None or not blueprint.has_attribute("role_name"):
            raise RuntimeError("configured vehicle blueprint is unavailable")
        blueprint.set_attribute("role_name", vehicle_config["role_name"])
        vehicle = world.try_spawn_actor(blueprint, spawn_points[start_index])
        if vehicle is None:
            raise RuntimeError(f"start spawn point {start_index} is occupied")
        world.tick(float(carla_config["timeout_seconds"]))

        if scenario_config is not None:
            obstacle_config = scenario_config["obstacle"]
            start_waypoint = carla_map.get_waypoint(
                spawn_points[start_index].location,
                project_to_road=True,
                lane_type=carla.LaneType.Driving,
            )
            if start_waypoint is None:
                raise RuntimeError("the scenario start is not on a driving lane")
            obstacle_waypoint = BRAKE.advance_waypoint(
                start_waypoint, float(obstacle_config["distance_m"])
            )
            obstacle_transform = obstacle_waypoint.transform
            obstacle_transform.location.z += float(
                obstacle_config["spawn_height_m"]
            )
            obstacle = BRAKE.spawn_vehicle(
                world,
                blueprint_library,
                obstacle_config["blueprint"],
                obstacle_config["role_name"],
                obstacle_transform,
            )
            obstacle.apply_control(
                carla.VehicleControl(throttle=0.0, brake=1.0, hand_brake=True)
            )
            obstacle.set_simulate_physics(False)
            collision_blueprint = blueprint_library.find(
                "sensor.other.collision"
            )
            if collision_blueprint is None:
                raise RuntimeError(
                    "CARLA collision sensor blueprint is unavailable"
                )
            collision_sensor = world.spawn_actor(
                collision_blueprint, carla.Transform(), attach_to=vehicle
            )
            collision_sensor.listen(
                lambda event: collision_frames.append(int(event.frame))
            )
            world.tick(float(carla_config["timeout_seconds"]))

        traffic_manager = None
        traffic_manager_port = None
        if autopilot_config is not None:
            traffic_manager_port = int(autopilot_config["traffic_manager_port"])
            traffic_manager = client.get_trafficmanager(traffic_manager_port)
            traffic_manager.set_synchronous_mode(True)
            traffic_manager.set_random_device_seed(int(autopilot_config["random_seed"]))
            traffic_manager.vehicle_percentage_speed_difference(
                vehicle, float(autopilot_config["speed_difference_percent"])
            )
            traffic_manager.auto_lane_change(
                vehicle, bool(autopilot_config["automatic_lane_change"])
            )
            with availability_lock:
                autopilot_unavailable = autopilot_unavailable_reason(
                    vehicle,
                    carla_map,
                    carla,
                    float(autopilot_config["maximum_road_distance_m"]),
                    float(autopilot_config["maximum_heading_error_degrees"]),
                )

        token = PROTOCOL.LocalControlServer.create_token_file(arguments.token_file)
        control_state = PROTOCOL.ExternalControlState(
            token,
            float(control_config["command_timeout_seconds"]),
            float(control_config["ownership_timeout_seconds"]),
            emit_control,
            validate_mode,
            available_modes,
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
            available_modes=sorted(available_modes),
            scenario=(
                {
                    "id": scenario_config["id"],
                    "obstacle_vehicle_id": obstacle.id,
                    "state": "ready",
                }
                if scenario_config is not None and obstacle is not None
                else None
            ),
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
        active_mode = "safe_stop"
        active_mode_generation = 0
        handover_started_at: Optional[float] = None
        handover_control = None
        manual_handover_seconds = float(
            control_config.get(
                "manual_handover_seconds",
                (
                    autopilot_config["manual_handover_seconds"]
                    if autopilot_config is not None
                    else 0.3
                ),
            )
        )
        scenario_machine = None
        scenario_started_at: Optional[float] = None
        scenario_frame_count = 0
        scenario_minimum_gap = math.inf
        scenario_peak_deceleration = 0.0
        scenario_stop_gap: Optional[float] = None
        scenario_finished = False

        def scenario_record(
            result: str, failure_reasons: list[str], recorded_at: float
        ) -> Dict[str, Any]:
            metrics = bounded_scenario_metrics(
                scenario_frame_count,
                (
                    recorded_at - scenario_started_at
                    if scenario_started_at is not None
                    else 0.0
                ),
                (
                    scenario_machine.brake_onset_speed_kmh
                    if scenario_machine is not None
                    else None
                ),
                scenario_peak_deceleration,
                scenario_minimum_gap,
                scenario_stop_gap,
                collision_frames,
            )
            return {
                "id": scenario_config["id"] if scenario_config else None,
                "mode_generation": active_mode_generation,
                "state": "complete",
                "result": result,
                "failure_reasons": failure_reasons,
                "metrics": metrics,
                "updated_at": utc_now(),
            }

        while not STOP_REQUESTED:
            now = time.monotonic()
            if now - started_at > float(control_config["maximum_session_seconds"]):
                completed = True
                break
            applied = control_state.current_control(now)
            with availability_lock:
                if autopilot_config is not None:
                    autopilot_unavailable = autopilot_unavailable_reason(
                        vehicle,
                        carla_map,
                        carla,
                        float(autopilot_config["maximum_road_distance_m"]),
                        float(autopilot_config["maximum_heading_error_degrees"]),
                    )
            mode_changed = (
                applied.mode != active_mode
                or applied.mode_generation != active_mode_generation
            )
            if mode_changed:
                previous_mode = active_mode
                if (
                    active_mode == "scenario"
                    and scenario_machine is not None
                    and not scenario_finished
                ):
                    aborted = scenario_record(
                        "ABORTED", ["operator changed the driving mode"], now
                    )
                    scenario_runs.append(aborted)
                    write_status(scenario=aborted, scenario_runs=scenario_runs)
                    emit("scenario_evaluated", **aborted)
                    scenario_finished = True
                if active_mode == "autopilot" and traffic_manager_port is not None:
                    handover_control = vehicle.get_control()
                    vehicle.set_autopilot(False, traffic_manager_port)
                if (
                    active_mode in {"autopilot", "scenario"}
                    and applied.mode == "manual"
                ):
                    handover_control = vehicle.get_control()
                    handover_started_at = now
                else:
                    handover_started_at = None
                if applied.mode == "autopilot":
                    if traffic_manager_port is None:
                        raise RuntimeError("autopilot mode selected without Traffic Manager")
                    handover_started_at = None
                    handover_control = None
                    vehicle.set_autopilot(True, traffic_manager_port)
                if applied.mode == "scenario":
                    if scenario_config is None or obstacle is None:
                        raise RuntimeError(
                            "scenario mode selected without a configured obstacle"
                        )
                    reset_scenario_vehicle(
                        carla, vehicle, spawn_points[start_index]
                    )
                    last_location = vehicle.get_location()
                    collision_frames.clear()
                    scenario_machine = BRAKE.BrakeScenarioStateMachine(
                        scenario_config, period
                    )
                    scenario_started_at = now
                    scenario_frame_count = 0
                    scenario_minimum_gap = math.inf
                    scenario_peak_deceleration = 0.0
                    scenario_stop_gap = None
                    scenario_finished = False
                    emit(
                        "scenario_started",
                        scenario_id=scenario_config["id"],
                        mode_generation=applied.mode_generation,
                    )
                active_mode = applied.mode
                active_mode_generation = applied.mode_generation
                emit(
                    "drive_mode_applied",
                    previous_mode=previous_mode,
                    mode=active_mode,
                    mode_generation=active_mode_generation,
                )
                write_status(
                    drive_mode=active_mode,
                    scenario=(
                        {
                            "id": scenario_config["id"],
                            "mode_generation": active_mode_generation,
                            "state": "running",
                            "phase": scenario_machine.phase,
                            "updated_at": utc_now(),
                        }
                        if active_mode == "scenario"
                        and scenario_config is not None
                        and scenario_machine is not None
                        else status.get("scenario")
                    ),
                )

            if active_mode == "scenario":
                if scenario_config is None or scenario_machine is None or obstacle is None:
                    raise RuntimeError("scenario mode is not initialized")
                scenario_elapsed = (
                    now - scenario_started_at
                    if scenario_started_at is not None
                    else 0.0
                )
                failure_reasons = []
                if collision_frames:
                    failure_reasons.append("a collision was recorded")
                if scenario_elapsed > float(
                    scenario_config["maximum_duration_seconds"]
                ):
                    failure_reasons.append("braking scenario duration exceeded")
                if failure_reasons and not scenario_finished:
                    failed = scenario_record("FAIL", failure_reasons, now)
                    scenario_runs.append(failed)
                    write_status(scenario=failed, scenario_runs=scenario_runs)
                    emit("scenario_evaluated", **failed)
                    scenario_finished = True
                    control_state.force_safe_stop("scenario_failed")
                    requested = carla.VehicleControl(
                        throttle=0.0, brake=1.0, steer=0.0
                    )
                elif scenario_finished:
                    requested = carla.VehicleControl(
                        throttle=0.0, brake=1.0, steer=0.0
                    )
                else:
                    vehicle_transform = vehicle.get_transform()
                    current_waypoint = carla_map.get_waypoint(
                        vehicle_transform.location,
                        project_to_road=True,
                        lane_type=carla.LaneType.Driving,
                    )
                    if current_waypoint is None:
                        raise RuntimeError(
                            "scenario vehicle left the driving lane"
                        )
                    target_waypoint = BRAKE.choose_forward_waypoint(
                        current_waypoint,
                        float(scenario_config["lookahead_distance_m"]),
                    )
                    gap = BRAKE.obstacle_gap_m(vehicle, obstacle)
                    scenario_control = scenario_machine.step(
                        BRAKE.speed_kmh(vehicle), gap
                    )
                    requested = carla.VehicleControl(
                        throttle=scenario_control.throttle,
                        brake=scenario_control.brake,
                        steer=BRAKE.steering_command(
                            vehicle_transform,
                            target_waypoint.transform.location,
                            float(scenario_config["maximum_steering"]),
                        ),
                    )
                vehicle.apply_control(requested)
            elif active_mode != "autopilot":
                requested = carla.VehicleControl(
                    throttle=applied.throttle,
                    brake=applied.brake,
                    steer=applied.steering,
                )
                if (
                    active_mode == "manual"
                    and handover_started_at is not None
                    and handover_control is not None
                ):
                    alpha = (now - handover_started_at) / manual_handover_seconds
                    requested = blend_control(carla, handover_control, requested, alpha)
                    if alpha >= 1.0:
                        handover_started_at = None
                        handover_control = None
                vehicle.apply_control(requested)
            if applied.safe_stop != last_safe_stop or applied.reason != last_reason:
                emit(
                    "control_applied",
                    sequence=applied.sequence,
                    safe_stop=applied.safe_stop,
                    reason=applied.reason,
                    mode=applied.mode,
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
            if (
                active_mode == "scenario"
                and scenario_config is not None
                and scenario_machine is not None
                and obstacle is not None
                and not scenario_finished
            ):
                scenario_frame_count += 1
                gap = BRAKE.obstacle_gap_m(vehicle, obstacle)
                scenario_minimum_gap = min(scenario_minimum_gap, gap)
                scenario_peak_deceleration = min(
                    scenario_peak_deceleration,
                    BRAKE.longitudinal_acceleration_mps2(vehicle),
                )
                if scenario_machine.phase == "HOLD" and scenario_stop_gap is None:
                    scenario_stop_gap = gap
                if scenario_machine.phase == "COMPLETE":
                    metrics = bounded_scenario_metrics(
                        scenario_frame_count,
                        (
                            now - scenario_started_at
                            if scenario_started_at is not None
                            else 0.0
                        ),
                        scenario_machine.brake_onset_speed_kmh,
                        scenario_peak_deceleration,
                        scenario_minimum_gap,
                        scenario_stop_gap,
                        collision_frames,
                    )
                    passed, failure_reasons = BRAKE.evaluate_result(
                        scenario_config, metrics
                    )
                    evaluated = {
                        "id": scenario_config["id"],
                        "mode_generation": active_mode_generation,
                        "state": "complete",
                        "result": "PASS" if passed else "FAIL",
                        "failure_reasons": failure_reasons,
                        "metrics": metrics,
                        "updated_at": utc_now(),
                    }
                    scenario_runs.append(evaluated)
                    write_status(
                        scenario=evaluated, scenario_runs=scenario_runs
                    )
                    emit("scenario_evaluated", **evaluated)
                    scenario_finished = True
                    control_state.force_safe_stop(
                        "scenario_complete" if passed else "scenario_failed"
                    )
                elif scenario_frame_count % 15 == 0:
                    write_status(
                        scenario={
                            "id": scenario_config["id"],
                            "mode_generation": active_mode_generation,
                            "state": "running",
                            "phase": scenario_machine.phase,
                            "frame_count": scenario_frame_count,
                            "speed_kmh": current_speed_kmh,
                            "obstacle_gap_m": gap,
                            "collision_count": len(collision_frames),
                            "updated_at": utc_now(),
                        }
                    )
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
            scenario_runs=scenario_runs,
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
        if collision_sensor is not None:
            try:
                if collision_sensor.is_listening:
                    collision_sensor.stop()
                collision_sensor.destroy()
                emit("collision_sensor_destroyed")
            except RuntimeError as error:
                emit("collision_sensor_cleanup_failed", error=str(error))
        if obstacle is not None:
            try:
                obstacle.destroy()
                emit("obstacle_destroyed", vehicle_id=obstacle.id)
            except RuntimeError as error:
                emit("obstacle_cleanup_failed", error=str(error))
        if vehicle is not None:
            try:
                if "traffic_manager_port" in locals() and traffic_manager_port is not None:
                    vehicle.set_autopilot(False, traffic_manager_port)
                vehicle.apply_control(
                    carla.VehicleControl(throttle=0.0, brake=1.0, steer=0.0)
                )
                vehicle.destroy()
                emit("vehicle_destroyed", vehicle_id=vehicle.id)
            except RuntimeError as error:
                emit("vehicle_cleanup_failed", error=str(error))
        if "traffic_manager" in locals() and traffic_manager is not None:
            try:
                traffic_manager.set_synchronous_mode(False)
                emit("traffic_manager_restored")
            except RuntimeError as error:
                emit("traffic_manager_restore_failed", error=str(error))
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
            scenario_runs=scenario_runs,
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
