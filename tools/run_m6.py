#!/usr/bin/env python3
"""Run the bounded M6 external-control and continuous-telemetry acceptance."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
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
M5 = load_module("m6_runner_m5", TOOLS / "run_m5.py")
CONTROLLER = load_module("m6_runner_config", TOOLS / "behavior_agent_controller.py")
STOP_REQUESTED = False
PORTABLE_UNIX_SOCKET_PATH_MAX = 103


def motion_is_verified(status: Optional[Dict[str, Any]]) -> bool:
    if status is None:
        return False
    motion = status.get("motion", {})
    return (
        float(motion.get("total_distance_m", 0.0)) >= 5.0
        and float(motion.get("maximum_speed_kmh", 0.0)) >= 5.0
        and float(motion.get("current_speed_kmh", float("inf"))) <= 0.5
    )


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def wait_for_control(
    status_file: Path,
    controller: Any,
    predicate: Any,
    description: str,
    timeout: float = 5.0,
) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if STOP_REQUESTED:
            raise InterruptedError("operator requested stop")
        status = M5.read_json(status_file)
        if status is not None and predicate(status.get("control", {})):
            return status
        if controller.process.poll() is not None:
            raise RuntimeError(
                f"external controller exited with {controller.process.returncode}"
            )
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {description}")


def client_command(
    arguments: argparse.Namespace,
    socket_file: Path,
    token_file: Path,
    scenario: str,
) -> list[str]:
    return [
        str(arguments.python),
        str(TOOLS / "external_control_client.py"),
        "--socket", str(socket_file),
        "--token", str(token_file),
        "--scenario", scenario,
        "--client-id", f"m6-{scenario}-client",
    ]


def create_control_paths() -> tuple[Path, Path, Path]:
    control_directory = Path(tempfile.mkdtemp(prefix="carla-m6-"))
    control_directory.chmod(0o700)
    socket_file = control_directory / "control.sock"
    if len(os.fsencode(socket_file)) > PORTABLE_UNIX_SOCKET_PATH_MAX:
        shutil.rmtree(control_directory, ignore_errors=True)
        raise RuntimeError(
            "local control socket path exceeds the portable Unix-domain limit"
        )
    return control_directory, socket_file, control_directory / "control.token"


def run_once(
    arguments: argparse.Namespace, config: Dict[str, Any], sequence: int
) -> bool:
    run_id = M5.utc_now().replace(":", "").replace("-", "") + "-" + uuid.uuid4().hex[:8]
    run_directory = arguments.run_root / run_id
    run_directory.mkdir(parents=True, exist_ok=False)
    effective_config = run_directory / "configuration.json"
    M5.atomic_write_json(effective_config, config)
    status_file = run_directory / "controller-status.json"
    gate_file = run_directory / "start.gate"
    stop_file = run_directory / "stop.gate"
    control_directory, socket_file, token_file = create_control_paths()
    manifest_path = run_directory / "manifest.json"
    log = M5.StructuredLog(run_directory / "events.jsonl")
    started_at = time.monotonic()

    controller_command = [
        str(arguments.python),
        str(TOOLS / "external_control_controller.py"),
        "--config", str(effective_config),
        "--python-api-root", str(arguments.python_api_root),
        "--status-file", str(status_file),
        "--gate-file", str(gate_file),
        "--stop-file", str(stop_file),
        "--socket-file", str(socket_file),
        "--token-file", str(token_file),
    ]
    runtime_command = M5.runtime_command(
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
        "started_at": M5.utc_now(),
        "control_source": "external_control",
        "configuration": config,
        "runtime_command": M5.public_runtime_options(runtime_command),
        "artifacts": {
            "events": "events.jsonl",
            "controller_status": "controller-status.json",
            "configuration": "configuration.json",
        },
    }
    M5.atomic_write_json(manifest_path, manifest)
    log.event("m6_run_started", run_id=run_id)

    controller: Optional[Any] = None
    runtime: Optional[Any] = None
    dashboard: Optional[Any] = None
    clients = []
    dashboard_health: Dict[str, Any] = {}
    success = False
    try:
        M5.progress(1, 8, "Preparing the external-control vehicle...")
        controller = M5.CapturedProcess(
            "external_controller", controller_command, log
        )
        ready = M5.wait_for_status(
            status_file,
            {"ready", "failed"},
            controller,
            float(config["simulation"]["startup_gate_timeout_seconds"]),
        )
        if ready["state"] != "ready":
            raise RuntimeError(f"external controller did not become ready: {ready}")

        M5.progress(2, 8, "Starting continuous VSS telemetry...")
        runtime = M5.CapturedProcess(
            "runtime", runtime_command, log, "VSS frame=", echo=False
        )
        if not runtime.ready.wait(
            float(config["simulation"]["startup_gate_timeout_seconds"])
        ):
            raise TimeoutError("runtime did not publish its first VSS frame")
        gate_file.touch()
        M5.progress(3, 8, "Verifying secure VISS telemetry...")
        if not M5.run_viss_probe(
            arguments.viss_client, config, arguments.certificate, log, "start"
        ):
            raise RuntimeError("independent VISS start probe failed")
        manifest["viss_start_probe"] = "passed"

        M5.progress(4, 8, "Opening telemetry health monitoring...")
        dashboard = M5.CapturedProcess(
            "dashboard",
            M5.dashboard_command(arguments.viss_client, config, arguments.certificate),
            log,
            "Connection        CONNECTED",
            echo=not arguments.dashboard_quiet,
            record_output=False,
            prefix_output=False,
        )
        if not dashboard.ready.wait(20):
            raise TimeoutError("dashboard did not become healthy")

        M5.progress(5, 8, "Driving with the independent control client...")
        acceptance = M5.CapturedProcess(
            "external_client_acceptance",
            client_command(arguments, socket_file, token_file, "acceptance"),
            log,
        )
        clients.append(acceptance)
        if acceptance.wait(30) != 0:
            raise RuntimeError("external-control acceptance client failed")
        timed_out = wait_for_control(
            status_file,
            controller,
            lambda control: control.get("command_timeouts", 0) >= 1
            and control.get("releases", 0) >= 1,
            "command timeout and release",
        )
        manifest["timeout_release_control"] = timed_out.get("control")

        M5.progress(6, 8, "Verifying disconnect safe stop...")
        disconnect = M5.CapturedProcess(
            "external_client_disconnect",
            client_command(arguments, socket_file, token_file, "disconnect"),
            log,
        )
        clients.append(disconnect)
        if disconnect.wait(15) != 0:
            raise RuntimeError("disconnect scenario client failed")
        disconnected = wait_for_control(
            status_file,
            controller,
            lambda control: control.get("disconnects", 0) >= 1
            and control.get("safe_stop_reason") == "disconnect",
            "disconnect safe stop",
        )
        manifest["disconnect_control"] = disconnected.get("control")

        M5.progress(7, 8, "Waiting for the bounded control session to finish...")
        session_status = M5.wait_for_status(
            status_file,
            {"session_complete", "failed"},
            controller,
            float(config["controller"]["external_control"]["maximum_session_seconds"])
            + 10,
        )
        if session_status["state"] != "session_complete":
            raise RuntimeError(f"external-control session failed: {session_status}")

        dashboard_health = dashboard.health_snapshot()
        dashboard.stop()
        dashboard = None
        if not M5.run_viss_probe(
            arguments.viss_client, config, arguments.certificate, log, "end"
        ):
            raise RuntimeError("independent VISS end probe failed")
        manifest["viss_end_probe"] = "passed"

        runtime_exit = runtime.stop()
        stop_file.touch()
        controller_exit = controller.wait(
            float(config["simulation"]["shutdown_gate_timeout_seconds"]) + 10
        )
        final_status = M5.read_json(status_file)
        success = (
            runtime_exit == 0
            and controller_exit == 0
            and final_status is not None
            and final_status.get("state") == "completed"
            and motion_is_verified(final_status)
        )
        manifest.update(
            {
                "status": "completed" if success else "failed",
                "finished_at": M5.utc_now(),
                "elapsed_seconds": time.monotonic() - started_at,
                "runtime_exit_code": runtime_exit,
                "controller_exit_code": controller_exit,
                "controller_final": final_status,
                "motion_verified": motion_is_verified(final_status),
                "dashboard_health": dashboard_health,
            }
        )
        M5.atomic_write_json(manifest_path, manifest)
        log.event("m6_run_finished", success=success)
        if success:
            M5.progress(8, 8, "M6 acceptance completed cleanly.")
        else:
            print("[8/8] M6 acceptance failed its final checks.", flush=True)
        return success
    except (
        OSError,
        RuntimeError,
        TimeoutError,
        subprocess.TimeoutExpired,
        InterruptedError,
    ) as error:
        log.event("m6_run_failed", error=str(error))
        manifest.update(
            {
                "status": "failed",
                "finished_at": M5.utc_now(),
                "elapsed_seconds": time.monotonic() - started_at,
                "error": str(error),
            }
        )
        M5.atomic_write_json(manifest_path, manifest)
        return False
    finally:
        for client in clients:
            if client.process.poll() is None:
                client.stop()
        if dashboard is not None and dashboard.process.poll() is None:
            dashboard.stop()
        if runtime is not None and runtime.process.poll() is None:
            runtime.stop()
        stop_file.touch(exist_ok=True)
        if controller is not None and controller.process.poll() is None:
            controller.stop()
        log.close()
        shutil.rmtree(control_directory, ignore_errors=True)
        print(f"M6 run artifacts: {run_directory}", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--viss-client", required=True, type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--run-root", type=Path, default=Path("runs"))
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--dashboard-quiet", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        config = CONTROLLER.load_config(arguments.config)
        if config["controller"]["type"] != "external_control":
            raise ValueError("M6 runner requires controller.type=external_control")
        if arguments.validate_only:
            print(f"M6 configuration is valid: {arguments.config}")
            return 0
        if arguments.repeat < 1 or arguments.repeat > 100:
            raise ValueError("--repeat must be between 1 and 100")
        for path in (
            arguments.runtime,
            arguments.viss_client,
            arguments.python,
            arguments.python_api_root,
            arguments.certificate,
            arguments.private_key,
        ):
            if not path.exists():
                raise ValueError(f"required path does not exist: {path}")
        arguments.run_root.mkdir(parents=True, exist_ok=True)
        for sequence in range(1, arguments.repeat + 1):
            if not run_once(arguments, config, sequence):
                return 2
        return 0
    except (ValueError, OSError) as error:
        print(f"M6 orchestration error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
