#!/usr/bin/env python3
"""Run the CARLA braking scenario, VISS gateway, and engineering dashboard."""

# Copyright (c) 2026 maninblack
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import importlib.util
import json
import signal
import subprocess
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
M5 = load_module("brake_demo_m5_runner", TOOLS / "run_m5.py")
CONFIG = load_module(
    "brake_demo_config", TOOLS / "behavior_agent_controller.py"
)
STOP_REQUESTED = threading.Event()


def request_stop(_signum: int, _frame: Any) -> None:
    STOP_REQUESTED.set()


def require_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise ValueError(f"{label} is not a file: {path}")
    return path.resolve()


def require_directory(path: Path, label: str) -> Path:
    if not path.is_dir():
        raise ValueError(f"{label} is not a directory: {path}")
    return path.resolve()


def wait_for_scenario_result(
    status_file: Path, controller: Any, timeout: float
) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if STOP_REQUESTED.is_set():
            raise InterruptedError("operator requested stop")
        status = M5.read_json(status_file)
        if status is not None and status.get("state") == "scenario_complete":
            return status
        if controller.process.poll() is not None:
            raise RuntimeError(
                f"scenario controller exited with {controller.process.returncode} "
                "before evaluation"
            )
        time.sleep(0.05)
    raise TimeoutError("timed out waiting for the braking scenario result")


def run(arguments: argparse.Namespace, config: Dict[str, Any]) -> bool:
    run_directory = arguments.run_directory
    run_directory.mkdir(parents=True, exist_ok=True)
    manifest_path = run_directory / "manifest.json"
    if manifest_path.exists():
        raise RuntimeError(f"run directory is already in use: {run_directory}")

    effective_config = run_directory / "configuration.json"
    M5.atomic_write_json(effective_config, config)
    status_file = run_directory / "scenario-status.json"
    gate_file = run_directory / "start.gate"
    stop_file = run_directory / "stop.gate"
    log = M5.StructuredLog(run_directory / "events.jsonl")

    controller_command = [
        str(arguments.python),
        str(TOOLS / "brake_event_scenario.py"),
        "--config",
        str(effective_config),
        "--python-api-root",
        str(arguments.python_api_root),
        "--status-file",
        str(status_file),
        "--gate-file",
        str(gate_file),
        "--stop-file",
        str(stop_file),
    ]
    runtime_command = M5.runtime_command(
        arguments.runtime,
        config,
        arguments.certificate,
        arguments.private_key,
        True,
    )
    dashboard_command = M5.dashboard_command(
        arguments.viss_client, config, arguments.certificate
    )
    manifest: Dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_directory.name,
        "status": "starting",
        "started_at": M5.utc_now(),
        "scenario_id": config["controller"]["scenario"]["id"],
        "configuration": config,
        "runtime_command": M5.public_runtime_options(runtime_command),
        "artifacts": {
            "configuration": "configuration.json",
            "events": "events.jsonl",
            "scenario_status": "scenario-status.json",
        },
    }
    M5.atomic_write_json(manifest_path, manifest)

    controller: Optional[Any] = None
    runtime: Optional[Any] = None
    dashboard: Optional[Any] = None
    dashboard_health: Dict[str, Any] = {}
    try:
        print("[1/5] Spawning the ego vehicle and stationary obstacle...", flush=True)
        controller = M5.CapturedProcess(
            "brake_scenario", controller_command, log
        )
        ready = M5.wait_for_status(
            status_file,
            {"ready", "failed"},
            controller,
            float(config["simulation"]["startup_gate_timeout_seconds"]),
        )
        if ready["state"] != "ready":
            raise RuntimeError(f"scenario controller did not become ready: {ready}")

        print("[2/5] Starting the VSS gateway and chase camera...", flush=True)
        runtime = M5.CapturedProcess(
            "runtime", runtime_command, log, "VSS frame=", echo=False
        )
        if not runtime.ready.wait(
            float(config["simulation"]["startup_gate_timeout_seconds"])
        ):
            raise TimeoutError("timed out waiting for the first VSS frame")
        if runtime.process.poll() is not None:
            raise RuntimeError("VSS runtime exited before its first frame")

        print("[3/5] Verifying VISS and opening the engineering dashboard...", flush=True)
        if not M5.run_viss_probe(
            arguments.viss_client,
            config,
            arguments.certificate,
            log,
            "start",
        ):
            raise RuntimeError("independent VISS start probe failed")
        dashboard = M5.CapturedProcess(
            "dashboard",
            dashboard_command,
            log,
            "Connection        CONNECTED",
            echo=not arguments.dashboard_quiet,
            record_output=False,
            prefix_output=False,
        )
        if not dashboard.ready.wait(20.0):
            raise TimeoutError("timed out waiting for the engineering dashboard")

        print("[4/5] Running the deterministic braking event...", flush=True)
        gate_file.touch()
        result = wait_for_scenario_result(
            status_file,
            controller,
            float(config["controller"]["scenario"]["maximum_duration_seconds"])
            + 10.0,
        )
        time.sleep(arguments.display_hold_seconds)
        dashboard_health = dashboard.health_snapshot()

        print(
            f"[5/5] Scenario {result.get('result', 'UNKNOWN')} — cleaning up...",
            flush=True,
        )
        if not M5.run_viss_probe(
            arguments.viss_client,
            config,
            arguments.certificate,
            log,
            "end",
        ):
            raise RuntimeError("independent VISS end probe failed")
        dashboard.stop()
        dashboard = None
        runtime_exit = runtime.stop()
        runtime = None
        stop_file.touch()
        controller_exit = controller.wait(
            float(config["simulation"]["shutdown_gate_timeout_seconds"]) + 5.0
        )
        controller = None
        success = (
            result.get("result") == "PASS"
            and runtime_exit == 0
            and controller_exit == 0
        )
        manifest.update(
            {
                "status": "completed" if success else "failed",
                "finished_at": M5.utc_now(),
                "scenario_result": result,
                "runtime_exit_code": runtime_exit,
                "controller_exit_code": controller_exit,
                "dashboard_health": dashboard_health,
            }
        )
        M5.atomic_write_json(manifest_path, manifest)
        return success
    except (
        InterruptedError,
        OSError,
        RuntimeError,
        subprocess.TimeoutExpired,
        TimeoutError,
    ) as error:
        manifest.update(
            {"status": "failed", "finished_at": M5.utc_now(), "error": str(error)}
        )
        M5.atomic_write_json(manifest_path, manifest)
        print(f"Brake-event demo error: {error}", file=sys.stderr, flush=True)
        return False
    finally:
        if dashboard is not None and dashboard.process.poll() is None:
            dashboard.stop()
        if runtime is not None and runtime.process.poll() is None:
            runtime.stop()
        if controller is not None and controller.process.poll() is None:
            stop_file.touch()
            controller.stop()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--viss-client", required=True, type=Path)
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--run-directory", required=True, type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--display-hold-seconds", type=float, default=3.0)
    parser.add_argument(
        "--dashboard-quiet",
        action="store_true",
        help="verify dashboard health without rendering it to this terminal",
    )
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    try:
        arguments = parse_arguments()
        arguments.config = require_file(arguments.config, "configuration")
        arguments.runtime = require_file(arguments.runtime, "runtime")
        arguments.viss_client = require_file(arguments.viss_client, "VISS client")
        arguments.python_api_root = require_directory(
            arguments.python_api_root, "CARLA Python API root"
        )
        arguments.certificate = require_file(arguments.certificate, "certificate")
        arguments.private_key = require_file(arguments.private_key, "private key")
        if arguments.display_hold_seconds < 0 or arguments.display_hold_seconds > 60:
            raise ValueError("--display-hold-seconds must be between 0 and 60")
        config = CONFIG.load_config(arguments.config)
        if config["controller"]["type"] != "brake_event_scenario":
            raise ValueError("controller.type must be brake_event_scenario")
        return 0 if run(arguments, config) else 2
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Brake-event demo error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
