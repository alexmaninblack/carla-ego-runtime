#!/usr/bin/env python3
"""Orchestrate one or more reproducible CARLA M5 route runs."""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import signal
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Dict, List, Optional, TextIO


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
        ready_text: Optional[str] = None
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
        self._thread = threading.Thread(target=self._read_output, daemon=True)
        self._thread.start()

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        for raw_line in self.process.stdout:
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            self._log.write(self.name, line)
            print(f"[{self.name}] {line}", flush=True)
            if self._ready_text and self._ready_text in line:
                self.ready.set()

    def wait(self, timeout: Optional[float] = None) -> int:
        return self.process.wait(timeout=timeout)

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
        status = read_json(path)
        if status is not None and status.get("state") in states:
            return status
        if process.process.poll() is not None:
            raise RuntimeError(
                f"{process.name} exited with {process.process.returncode} before {states}"
            )
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {states} in {path}")


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
    status_file = run_directory / "controller-status.json"
    gate_file = run_directory / "start.gate"
    stop_file = run_directory / "stop.gate"
    manifest_path = run_directory / "manifest.json"
    structured_log = StructuredLog(run_directory / "events.jsonl")
    started_at = time.monotonic()

    controller_command = [
        str(arguments.python),
        str(Path(__file__).with_name("behavior_agent_controller.py")),
        "--config", str(arguments.config),
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
        },
    }
    atomic_write_json(manifest_path, manifest)
    structured_log.event("run_started", run_id=run_id, sequence=sequence)

    controller: Optional[CapturedProcess] = None
    telemetry: Optional[CapturedProcess] = None
    success = False
    try:
        controller = CapturedProcess(
            "behavior_agent", controller_command, structured_log
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

        telemetry = CapturedProcess(
            "runtime", runtime, structured_log, "VSS frame="
        )
        runtime_deadline = time.monotonic() + float(
            config["simulation"]["startup_gate_timeout_seconds"]
        )
        while not telemetry.ready.wait(0.05):
            if telemetry.process.poll() is not None:
                raise RuntimeError(
                    f"runtime exited with {telemetry.process.returncode} during startup"
                )
            if time.monotonic() >= runtime_deadline:
                raise TimeoutError("runtime did not publish its first VSS frame")
        gate_file.touch()
        manifest["status"] = "driving"
        manifest["driving_started_at"] = utc_now()
        atomic_write_json(manifest_path, manifest)
        structured_log.event("startup_gate_opened")
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

        route_status = wait_for_status(
            status_file,
            {"route_complete", "failed"},
            controller,
            float(config["simulation"]["maximum_route_seconds"]) + 30,
        )
        if route_status["state"] != "route_complete":
            raise RuntimeError(f"route failed: {route_status}")

        if not run_viss_probe(
            arguments.viss_client,
            config,
            arguments.certificate,
            structured_log,
            "end",
        ):
            raise RuntimeError("independent VISS end probe failed")
        manifest["viss_end_probe"] = "passed"
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
    except (OSError, RuntimeError, TimeoutError, subprocess.TimeoutExpired) as error:
        structured_log.event("run_failed", error=str(error))
        manifest.update(
            {
                "status": "failed",
                "finished_at": utc_now(),
                "elapsed_seconds": time.monotonic() - started_at,
                "error": str(error),
            }
        )
        atomic_write_json(manifest_path, manifest)
        return False
    finally:
        if telemetry is not None and telemetry.process.poll() is None:
            telemetry.stop()
        stop_file.touch(exist_ok=True)
        if controller is not None and controller.process.poll() is None:
            controller.stop()
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
    parser.add_argument("--headless", action="store_true")
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
    arguments = parse_arguments()
    try:
        config = CONTROLLER.load_config(arguments.config)
        if arguments.validate_only:
            print(f"M5 configuration is valid: {arguments.config}")
            return 0
        require_live_paths(arguments)
        arguments.run_root.mkdir(parents=True, exist_ok=True)
        for sequence in range(1, arguments.repeat + 1):
            if not run_once(arguments, config, sequence):
                return 2
        return 0
    except (ValueError, OSError) as error:
        print(f"M5 orchestration error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
