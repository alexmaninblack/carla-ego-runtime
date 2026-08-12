#!/usr/bin/env python3
"""Orchestrate one or more reproducible CARLA M5 route runs."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import re
import signal
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, TextIO


STOP_REQUESTED = threading.Event()
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def collect_dashboard_health(line: str, health: Dict[str, Any]) -> Optional[str]:
    clean = ANSI_ESCAPE.sub("", line).strip()
    fields = {
        "Connection": "connection",
        "Data health": "data_health",
        "Simulation rate": "simulation_hz",
        "Dashboard rate": "delivery_hz",
        "VISS latency": "event_latency_ms",
        "Events received": "events_received",
    }
    for label, key in fields.items():
        if not clean.startswith(label):
            continue
        value = clean[len(label):].strip()
        if key in {"simulation_hz", "delivery_hz", "event_latency_ms"}:
            match = re.match(r"([0-9]+(?:\.[0-9]+)?)", value)
            health[key] = float(match.group(1)) if match else None
        elif key == "events_received":
            health[key] = int(value) if value.isdigit() else None
        else:
            health[key] = value
        health["captured_at"] = utc_now()
        return key
    return None


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def load_controller_module() -> Any:
    path = Path(__file__).with_name("behavior_agent_controller.py")
    specification = importlib.util.spec_from_file_location("m5_behavior_agent", path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


CONTROLLER = load_controller_module()


def atomic_write_json(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def apply_run_overrides(
    config: Dict[str, Any], route_cycles: Optional[int],
    maximum_route_seconds: Optional[float] = None,
) -> Dict[str, Any]:
    """Create a validated effective configuration for bounded endurance runs."""
    effective = json.loads(json.dumps(config))
    if route_cycles is not None:
        if route_cycles < 1 or route_cycles > 100:
            raise ValueError("--route-cycles must be between 1 and 100")
        base_cycles = int(effective["route"]["cycles"])
        scale = route_cycles / base_cycles
        effective["route"]["cycles"] = route_cycles
        effective["simulation"]["maximum_route_seconds"] = min(
            86400,
            max(
                10,
                int(effective["simulation"]["maximum_route_seconds"] * scale),
            ),
        )
    if maximum_route_seconds is not None:
        if maximum_route_seconds < 10 or maximum_route_seconds > 86400:
            raise ValueError(
                "--maximum-route-seconds must be between 10 and 86400"
            )
        effective["simulation"]["maximum_route_seconds"] = (
            maximum_route_seconds
        )
    return CONTROLLER.validate_config(effective)


def read_json(path: Path) -> Optional[Dict[str, Any]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


class StructuredLog:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._file: TextIO = path.open("a", encoding="utf-8")
        self._lock = threading.Lock()

    def write(self, source: str, line: str) -> Dict[str, Any]:
        try:
            parsed = json.loads(line)
            record = parsed if isinstance(parsed, dict) else {"message": line}
        except json.JSONDecodeError:
            record = {"event": "message", "message": line}
        record.setdefault("ts", utc_now())
        record.setdefault("source", source)
        with self._lock:
            self._file.write(json.dumps(record, sort_keys=True) + "\n")
            self._file.flush()
        return record

    def event(self, event: str, **fields: Any) -> None:
        record = {"ts": utc_now(), "source": "orchestrator", "event": event}
        record.update(fields)
        with self._lock:
            self._file.write(json.dumps(record, sort_keys=True) + "\n")
            self._file.flush()

    def close(self) -> None:
        self._file.close()


class CapturedProcess:
    def __init__(
        self, name: str, command: List[str], log: StructuredLog,
        ready_text: Optional[str] = None, *, echo: bool = True,
        record_output: bool = True, prefix_output: bool = True
    ):
        self.name = name
        self.command = command
        self.ready = threading.Event()
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        self._log = log
        self._ready_text = ready_text
        self._echo = echo
        self._record_output = record_output
        self._prefix_output = prefix_output
        self._health: Dict[str, Any] = {}
        self._health_lock = threading.Lock()
        self._first_health: Dict[str, Any] = {}
        self._health_samples = 0
        self._simulation_hz_sum = 0.0
        self._simulation_hz_min: Optional[float] = None
        self._simulation_hz_max: Optional[float] = None
        self._latency_sum = 0.0
        self._latency_samples = 0
        self._latency_min: Optional[float] = None
        self._latency_max: Optional[float] = None
        self._thread = threading.Thread(target=self._read_output, daemon=True)
        self._thread.start()

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        for raw_line in self.process.stdout:
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            if self.name == "dashboard":
                with self._health_lock:
                    updated_key = collect_dashboard_health(line, self._health)
                    if (
                        updated_key == "events_received"
                        and self._health.get("events_received") == 1
                        and not self._first_health
                    ):
                        self._first_health = dict(self._health)
                    if updated_key == "event_latency_ms":
                        latency = self._health["event_latency_ms"]
                        if isinstance(latency, float):
                            self._latency_samples += 1
                            self._latency_sum += latency
                            self._latency_min = (
                                latency if self._latency_min is None
                                else min(self._latency_min, latency)
                            )
                            self._latency_max = (
                                latency if self._latency_max is None
                                else max(self._latency_max, latency)
                            )
                    if updated_key == "simulation_hz":
                        simulation_hz = self._health["simulation_hz"]
                        if isinstance(simulation_hz, float):
                            self._health_samples += 1
                            self._simulation_hz_sum += simulation_hz
                            self._simulation_hz_min = (
                                simulation_hz if self._simulation_hz_min is None
                                else min(self._simulation_hz_min, simulation_hz)
                            )
                            self._simulation_hz_max = (
                                simulation_hz if self._simulation_hz_max is None
                                else max(self._simulation_hz_max, simulation_hz)
                            )
            if self._record_output:
                self._log.write(self.name, line)
            if self._echo:
                print(
                    f"[{self.name}] {line}" if self._prefix_output else line,
                    flush=True,
                )
            if self._ready_text and self._ready_text in line:
                self.ready.set()

    def wait(self, timeout: Optional[float] = None) -> int:
        return self.process.wait(timeout=timeout)

    def health_snapshot(self) -> Dict[str, Any]:
        with self._health_lock:
            snapshot = dict(self._health)
            snapshot["first"] = dict(self._first_health)
            snapshot["samples"] = self._health_samples
            if self._health_samples:
                snapshot["simulation_hz_average"] = (
                    self._simulation_hz_sum / self._health_samples
                )
                snapshot["simulation_hz_minimum"] = self._simulation_hz_min
                snapshot["simulation_hz_maximum"] = self._simulation_hz_max
            if self._latency_min is not None:
                snapshot["event_latency_ms_average"] = (
                    self._latency_sum / self._latency_samples
                )
                snapshot["event_latency_ms_minimum"] = self._latency_min
                snapshot["event_latency_ms_maximum"] = self._latency_max
            return snapshot

    def stop(self, timeout: float = 20) -> int:
        if self.process.poll() is not None:
            return int(self.process.returncode)
        self.process.send_signal(signal.SIGINT)
        try:
            return self.wait(timeout)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                return self.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                return self.wait(5)


def wait_for_status(
    path: Path, states: set[str], process: CapturedProcess, timeout: float
) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if STOP_REQUESTED.is_set():
            raise InterruptedError("operator requested stop")
        status = read_json(path)
        if status is not None and status.get("state") in states:
            return status
        if process.process.poll() is not None:
            raise RuntimeError(
                f"{process.name} exited with {process.process.returncode} before {states}"
            )
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {states} in {path}")


def progress(stage: int, total: int, message: str) -> None:
    print(f"[{stage}/{total}] {message}", flush=True)


def dashboard_command(
    client: Path, config: Dict[str, Any], certificate: Path
) -> List[str]:
    runtime = config["runtime"]
    return [
        str(client),
        "--host", "localhost",
        "--port", str(runtime["viss_port"]),
        "--ca", str(certificate),
        "--monitor",
        "--monitor-period-ms", str(runtime["dashboard_period_ms"]),
    ]


def runtime_command(
    executable: Path, config: Dict[str, Any], certificate: Path, private_key: Path,
    visual: bool
) -> List[str]:
    carla_config = config["carla"]
    vehicle = config["vehicle"]
    runtime = config["runtime"]
    command = [
        str(executable),
        "--host", str(carla_config["host"]),
        "--port", str(carla_config["port"]),
        "--timeout-ms", str(int(float(carla_config["timeout_seconds"]) * 1000)),
        "--role-name", str(vehicle["role_name"]),
        "--no-spawn",
        "--observe-ticks",
        "--max-frames", "0",
        "--gnss-sensor-tick-seconds", "0.1",
        "--gnss-max-age-seconds", "0.25",
        "--log-every-frames", str(runtime["log_every_frames"]),
        "--viss",
        "--viss-bind-address", str(runtime["viss_bind_address"]),
        "--viss-port", str(runtime["viss_port"]),
        "--viss-cert", str(certificate),
        "--viss-key", str(private_key),
    ]
    if visual and runtime["chase_camera"]:
        command.extend(
            [
                "--chase-camera",
                "--chase-camera-response", str(runtime["chase_camera_response"]),
                "--chase-camera-update-hz", str(runtime["chase_camera_update_hz"]),
                "--exposure-offset", str(runtime["exposure_offset"]),
            ]
        )
    return command


def public_runtime_options(command: List[str]) -> List[str]:
    """Return runtime options suitable for a manifest without TLS file paths."""
    redacted: List[str] = []
    skip_next = False
    for index, argument in enumerate(command):
        if skip_next:
            skip_next = False
            continue
        if argument in {"--viss-cert", "--viss-key"}:
            redacted.extend([argument, "<redacted-local-path>"])
            skip_next = True
        else:
            redacted.append(argument)
    return redacted


def run_viss_probe(
    client: Path, config: Dict[str, Any], certificate: Path,
    log: StructuredLog, phase: str
) -> bool:
    runtime = config["runtime"]
    if phase == "start":
        request = json.dumps(
            {
                "action": "subscribe",
                "path": "Vehicle",
                "filter": [
                    {
                        "variant": "paths",
                        "parameter": ["Speed", "CurrentLocation.*", "CarlaSimulation.FrameId"],
                    },
                    {"variant": "timebased", "parameter": {"period": "100"}},
                ],
                "requestId": "m5-start-probe",
            },
            separators=(",", ":"),
        )
        message_count = "3"
    else:
        request = json.dumps(
            {
                "action": "get",
                "path": "Vehicle.CarlaSimulation.FrameId",
                "requestId": "m5-end-probe",
            },
            separators=(",", ":"),
        )
        message_count = "1"
    command = [
        str(client),
        "--host", "localhost",
        "--port", str(runtime["viss_port"]),
        "--ca", str(certificate),
        "--messages", message_count,
        "--request", request,
    ]
    result = subprocess.run(
        command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=20
    )
    for line in result.stdout.splitlines():
        if line:
            log.write(f"viss_{phase}_probe", line)
    log.event("viss_probe_finished", phase=phase, exit_code=result.returncode)
    return result.returncode == 0


def run_once(
    arguments: argparse.Namespace, config: Dict[str, Any], sequence: int
) -> bool:
    run_id = utc_now().replace(":", "").replace("-", "") + "-" + uuid.uuid4().hex[:8]
    run_directory = arguments.run_root / run_id
    run_directory.mkdir(parents=True, exist_ok=False)
    effective_config_path = run_directory / "configuration.json"
    atomic_write_json(effective_config_path, config)
    status_file = run_directory / "controller-status.json"
    gate_file = run_directory / "start.gate"
    stop_file = run_directory / "stop.gate"
    manifest_path = run_directory / "manifest.json"
    structured_log = StructuredLog(run_directory / "events.jsonl")
    started_at = time.monotonic()

    controller_command = [
        str(arguments.python),
        str(Path(__file__).with_name("behavior_agent_controller.py")),
        "--config", str(effective_config_path),
        "--python-api-root", str(arguments.python_api_root),
        "--status-file", str(status_file),
        "--gate-file", str(gate_file),
        "--stop-file", str(stop_file),
    ]
    runtime = runtime_command(
        arguments.runtime,
        config,
        arguments.certificate,
        arguments.private_key,
        not arguments.headless,
    )
    manifest: Dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_id,
        "sequence": sequence,
        "status": "starting",
        "started_at": utc_now(),
        "configuration": config,
        "control_source": "behavior_agent",
        "runtime_command": public_runtime_options(runtime),
        "artifacts": {
            "events": "events.jsonl",
            "controller_status": "controller-status.json",
            "configuration": "configuration.json",
        },
    }
    atomic_write_json(manifest_path, manifest)
    structured_log.event("run_started", run_id=run_id, sequence=sequence)

    controller: Optional[CapturedProcess] = None
    telemetry: Optional[CapturedProcess] = None
    dashboard: Optional[CapturedProcess] = None
    dashboard_health: Dict[str, Any] = {}
    success = False
    try:
        progress(1, 7, "Preparing the CARLA vehicle and route...")
        controller = CapturedProcess(
            "behavior_agent", controller_command, structured_log,
            echo=not arguments.dashboard
        )
        ready = wait_for_status(
            status_file,
            {"ready", "failed"},
            controller,
            float(config["simulation"]["startup_gate_timeout_seconds"]),
        )
        if ready["state"] != "ready":
            raise RuntimeError(f"controller did not become ready: {ready}")
        manifest["controller_ready"] = ready
        manifest["status"] = "controller_ready"
        atomic_write_json(manifest_path, manifest)

        progress(2, 7, "Vehicle is ready; starting VSS telemetry...")
        telemetry = CapturedProcess(
            "runtime", runtime, structured_log, "VSS frame=",
            echo=not arguments.dashboard
        )
        runtime_deadline = time.monotonic() + float(
            config["simulation"]["startup_gate_timeout_seconds"]
        )
        while not telemetry.ready.wait(0.05):
            if STOP_REQUESTED.is_set():
                raise InterruptedError("operator requested stop")
            if telemetry.process.poll() is not None:
                raise RuntimeError(
                    f"runtime exited with {telemetry.process.returncode} during startup"
                )
            if time.monotonic() >= runtime_deadline:
                raise TimeoutError("runtime did not publish its first VSS frame")
        progress(3, 7, "Telemetry is live; opening the routed drive...")
        gate_file.touch()
        manifest["status"] = "driving"
        manifest["driving_started_at"] = utc_now()
        atomic_write_json(manifest_path, manifest)
        structured_log.event("startup_gate_opened")
        progress(4, 7, "Verifying the secure VISS connection...")
        if not run_viss_probe(
            arguments.viss_client,
            config,
            arguments.certificate,
            structured_log,
            "start",
        ):
            raise RuntimeError("independent VISS start probe failed")
        manifest["viss_start_probe"] = "passed"
        atomic_write_json(manifest_path, manifest)

        if arguments.dashboard:
            progress(5, 7, "Opening the live telemetry dashboard...")
            dashboard = CapturedProcess(
                "dashboard",
                dashboard_command(
                    arguments.viss_client, config, arguments.certificate
                ),
                structured_log,
                "Connection        CONNECTED",
                echo=not arguments.dashboard_quiet,
                record_output=False,
                prefix_output=False,
            )
            dashboard_deadline = time.monotonic() + 20.0
            while not dashboard.ready.wait(0.05):
                if STOP_REQUESTED.is_set():
                    raise InterruptedError("operator requested stop")
                if dashboard.process.poll() is not None:
                    raise RuntimeError(
                        "dashboard exited before receiving live telemetry"
                    )
                if time.monotonic() >= dashboard_deadline:
                    raise TimeoutError("dashboard did not receive live telemetry")
            manifest["dashboard"] = "connected"
            atomic_write_json(manifest_path, manifest)
        else:
            progress(5, 7, "VISS connection verified.")
        progress(6, 7, "All systems are healthy.")
        progress(7, 7, "Driving. Press Ctrl-C once for a clean stop.")

        route_status = wait_for_status(
            status_file,
            {"route_complete", "failed"},
            controller,
            float(config["simulation"]["maximum_route_seconds"]) + 30,
        )
        if route_status["state"] != "route_complete":
            raise RuntimeError(f"route failed: {route_status}")

        if dashboard is not None:
            dashboard_health = dashboard.health_snapshot()
            dashboard.stop()
            dashboard = None
            print("\nRoute complete; closing the dashboard...", flush=True)

        if not run_viss_probe(
            arguments.viss_client,
            config,
            arguments.certificate,
            structured_log,
            "end",
        ):
            raise RuntimeError("independent VISS end probe failed")
        manifest["viss_end_probe"] = "passed"
        if dashboard_health:
            manifest["dashboard_health"] = dashboard_health
        atomic_write_json(manifest_path, manifest)

        runtime_exit = telemetry.stop()
        stop_file.touch()
        controller_exit = controller.wait(
            float(config["simulation"]["shutdown_gate_timeout_seconds"]) + 10
        )
        final_status = read_json(status_file)
        success = (
            runtime_exit == 0
            and controller_exit == 0
            and final_status is not None
            and final_status.get("state") == "completed"
        )
        manifest.update(
            {
                "status": "completed" if success else "failed",
                "finished_at": utc_now(),
                "elapsed_seconds": time.monotonic() - started_at,
                "runtime_exit_code": runtime_exit,
                "controller_exit_code": controller_exit,
                "controller_final": final_status,
            }
        )
        atomic_write_json(manifest_path, manifest)
        structured_log.event("run_finished", success=success)
        return success
    except (OSError, RuntimeError, TimeoutError, subprocess.TimeoutExpired,
            InterruptedError) as error:
        stopped = isinstance(error, InterruptedError)
        structured_log.event("run_stopped" if stopped else "run_failed",
                             error=str(error))
        manifest.update(
            {
                "status": "stopped" if stopped else "failed",
                "finished_at": utc_now(),
                "elapsed_seconds": time.monotonic() - started_at,
                "error": str(error),
            }
        )
        atomic_write_json(manifest_path, manifest)
        return False
    finally:
        dashboard_exit: Optional[int] = None
        runtime_exit: Optional[int] = None
        controller_exit: Optional[int] = None
        if dashboard is not None and dashboard.process.poll() is None:
            dashboard_health = dashboard.health_snapshot()
            dashboard_exit = dashboard.stop()
        elif dashboard is not None:
            dashboard_health = dashboard.health_snapshot()
            dashboard_exit = dashboard.process.returncode
        if telemetry is not None and telemetry.process.poll() is None:
            runtime_exit = telemetry.stop()
        elif telemetry is not None:
            runtime_exit = telemetry.process.returncode
        stop_file.touch(exist_ok=True)
        if controller is not None and controller.process.poll() is None:
            controller_exit = controller.stop()
        elif controller is not None:
            controller_exit = controller.process.returncode
        if manifest.get("status") != "completed":
            manifest.update(
                {
                    "finished_at": utc_now(),
                    "dashboard_exit_code": dashboard_exit,
                    "runtime_exit_code": runtime_exit,
                    "controller_exit_code": controller_exit,
                    "controller_final": read_json(status_file),
                    "dashboard_health": dashboard_health or None,
                }
            )
            atomic_write_json(manifest_path, manifest)
        structured_log.close()
        print(f"M5 run artifacts: {run_directory}", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--viss-client", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-api-root", type=Path)
    parser.add_argument("--certificate", type=Path)
    parser.add_argument("--private-key", type=Path)
    parser.add_argument("--run-root", type=Path, default=Path("runs"))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument(
        "--route-cycles", type=int,
        help="override route cycles and scale the safety timeout for endurance",
    )
    parser.add_argument("--maximum-route-seconds", type=float)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument(
        "--dashboard", action="store_true",
        help="show the live VSS health dashboard during the route",
    )
    parser.add_argument(
        "--dashboard-quiet", action="store_true",
        help="collect dashboard health without rendering it to stdout",
    )
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def require_live_paths(arguments: argparse.Namespace) -> None:
    required = {
        "--runtime": arguments.runtime,
        "--viss-client": arguments.viss_client,
        "--python-api-root": arguments.python_api_root,
        "--certificate": arguments.certificate,
        "--private-key": arguments.private_key,
    }
    for option, path in required.items():
        if path is None:
            raise ValueError(f"{option} is required for a live run")
        if not path.exists():
            raise ValueError(f"{option} does not exist: {path}")
    if arguments.repeat < 1 or arguments.repeat > 100:
        raise ValueError("--repeat must be between 1 and 100")


def main() -> int:
    STOP_REQUESTED.clear()
    signal.signal(signal.SIGINT, lambda _signum, _frame: STOP_REQUESTED.set())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: STOP_REQUESTED.set())
    arguments = parse_arguments()
    try:
        config = CONTROLLER.load_config(arguments.config)
        if arguments.validate_only:
            print(f"M5 configuration is valid: {arguments.config}")
            return 0
        config = apply_run_overrides(
            config, arguments.route_cycles, arguments.maximum_route_seconds
        )
        require_live_paths(arguments)
        arguments.run_root.mkdir(parents=True, exist_ok=True)
        for sequence in range(1, arguments.repeat + 1):
            if not run_once(arguments, config, sequence):
                if STOP_REQUESTED.is_set():
                    print("M5 session stopped cleanly by the operator.", flush=True)
                    return 130
                return 2
        return 0
    except (ValueError, OSError, KeyboardInterrupt) as error:
        print(f"M5 orchestration error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
