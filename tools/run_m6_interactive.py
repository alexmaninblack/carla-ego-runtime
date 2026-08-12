#!/usr/bin/env python3
"""Run one interactive M6.1 keyboard drive with a live terminal dashboard."""

from __future__ import annotations

import argparse
import fcntl
import importlib.util
import json
import shlex
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
M5 = load_module("m61_runner_m5", TOOLS / "run_m5.py")
CONTROLLER = load_module("m61_runner_config", TOOLS / "behavior_agent_controller.py")
STOP_REQUESTED = threading.Event()


def request_stop(_signum: int, _frame: Any) -> None:
    STOP_REQUESTED.set()


def timeline_mark(path: Path, started_at: float, stage: str, **fields: Any) -> None:
    lock_path = path.with_suffix(path.suffix + ".lock")
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            try:
                payload = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                payload = {"schema_version": 1, "stages": []}
            stages = payload.setdefault("stages", [])
            stages.append(
                {
                    "stage": stage,
                    "ts": M5.utc_now(),
                    "elapsed_seconds": time.time() - started_at,
                    **fields,
                }
            )
            M5.atomic_write_json(path, payload)
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def wait_until_ready(process: Any, timeout: float, description: str) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if STOP_REQUESTED.is_set():
            raise InterruptedError("operator requested stop")
        if process.ready.wait(0.05):
            return
        if process.process.poll() is not None:
            raise RuntimeError(
                f"{process.name} exited with {process.process.returncode} "
                f"before {description}"
            )
    raise TimeoutError(f"timed out waiting for {description}")


def keyboard_command(
    arguments: argparse.Namespace, socket_file: Path, token_file: Path
) -> list[str]:
    bridge = [
        str(arguments.python),
        str(TOOLS / "keyboard_control_bridge.py"),
        "--socket",
        str(socket_file),
        "--token",
        str(token_file),
        "--client-id",
        "m6-keyboard-client",
    ]
    return [
        str(arguments.keyboard_ui),
        shlex.join(bridge),
    ]


def run(arguments: argparse.Namespace, config: Dict[str, Any]) -> bool:
    run_directory = arguments.run_directory
    run_directory.mkdir(parents=True, exist_ok=True)
    effective_config = run_directory / "configuration.json"
    M5.atomic_write_json(effective_config, config)
    status_file = run_directory / "controller-status.json"
    gate_file = run_directory / "start.gate"
    socket_file = run_directory / "control.sock"
    token_file = run_directory / "control.token"
    timeline_file = run_directory / "startup-timeline.json"
    manifest_path = run_directory / "manifest.json"
    if manifest_path.exists():
        raise RuntimeError(f"run directory is already in use: {run_directory}")
    log = M5.StructuredLog(run_directory / "events.jsonl")
    started_at = arguments.started_timestamp
    timeline_mark(timeline_file, started_at, "interactive_orchestrator_started")

    controller_command = [
        str(arguments.python),
        str(TOOLS / "external_control_controller.py"),
        "--config",
        str(effective_config),
        "--python-api-root",
        str(arguments.python_api_root),
        "--status-file",
        str(status_file),
        "--gate-file",
        str(gate_file),
        "--socket-file",
        str(socket_file),
        "--token-file",
        str(token_file),
    ]
    runtime_command = M5.runtime_command(
        arguments.runtime,
        config,
        arguments.certificate,
        arguments.private_key,
        True,
    )
    manifest: Dict[str, Any] = {
        "schema_version": 1,
        "run_id": run_directory.name,
        "status": "starting",
        "started_at": M5.utc_now(),
        "control_source": "keyboard_external_control",
        "configuration": config,
        "runtime_command": M5.public_runtime_options(runtime_command),
        "artifacts": {
            "events": "events.jsonl",
            "controller_status": "controller-status.json",
            "configuration": "configuration.json",
            "startup_timeline": "startup-timeline.json",
        },
    }
    M5.atomic_write_json(manifest_path, manifest)

    controller: Optional[Any] = None
    runtime: Optional[Any] = None
    dashboard: Optional[Any] = None
    keyboard: Optional[Any] = None
    keyboard_exit: Optional[int] = None
    dashboard_health: Dict[str, Any] = {}
    runtime_exit: Optional[int] = None
    controller_exit: Optional[int] = None
    success = False
    try:
        print("[1/6] Preparing the manual-drive vehicle...", flush=True)
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
        timeline_mark(
            timeline_file,
            started_at,
            "vehicle_ready",
            vehicle_id=ready.get("vehicle_id"),
        )

        print("[2/6] Starting VSS telemetry and chase camera...", flush=True)
        runtime = M5.CapturedProcess(
            "runtime", runtime_command, log, "VSS frame=", echo=False
        )
        wait_until_ready(
            runtime,
            float(config["simulation"]["startup_gate_timeout_seconds"]),
            "the first VSS frame",
        )
        timeline_mark(timeline_file, started_at, "first_vss_frame")
        gate_file.touch()

        print("[3/6] Verifying secure VISS telemetry...", flush=True)
        if not M5.run_viss_probe(
            arguments.viss_client, config, arguments.certificate, log, "start"
        ):
            raise RuntimeError("independent VISS start probe failed")
        timeline_mark(timeline_file, started_at, "viss_verified")

        print("[4/6] Opening the live VSS dashboard in this terminal...", flush=True)
        dashboard = M5.CapturedProcess(
            "dashboard",
            M5.dashboard_command(arguments.viss_client, config, arguments.certificate),
            log,
            "Connection        CONNECTED",
            echo=True,
            record_output=False,
            prefix_output=False,
        )
        wait_until_ready(dashboard, 20.0, "the live VSS dashboard")
        timeline_mark(timeline_file, started_at, "dashboard_ready")

        print("[5/6] Opening keyboard control...", flush=True)
        keyboard = M5.CapturedProcess(
            "keyboard",
            keyboard_command(arguments, socket_file, token_file),
            log,
            "keyboard_ui_ready",
            echo=True,
        )
        wait_until_ready(keyboard, 10.0, "the keyboard-control window")
        timeline_mark(timeline_file, started_at, "keyboard_ready")
        print(
            "[6/6] READY — press Enter in the control window, then use the arrows.",
            flush=True,
        )

        while keyboard.process.poll() is None:
            if STOP_REQUESTED.wait(0.1):
                keyboard.stop()
                break
            if controller.process.poll() is not None:
                raise RuntimeError("external controller stopped during manual drive")
            if runtime.process.poll() is not None:
                raise RuntimeError("telemetry runtime stopped during manual drive")
            if dashboard.process.poll() is not None:
                raise RuntimeError("VSS dashboard stopped during manual drive")
        keyboard_exit = int(keyboard.process.returncode)
        timeline_mark(
            timeline_file,
            started_at,
            "keyboard_session_finished",
            exit_code=keyboard_exit,
        )

        dashboard_health = dashboard.health_snapshot()
        dashboard.stop()
        dashboard = None
        if not M5.run_viss_probe(
            arguments.viss_client, config, arguments.certificate, log, "end"
        ):
            raise RuntimeError("independent VISS end probe failed")
        runtime_exit = runtime.stop()
        runtime = None
        controller_exit = controller.stop()
        controller = None
        final_status = M5.read_json(status_file)
        success = (
            keyboard_exit == 0
            and runtime_exit == 0
            and controller_exit == 0
            and final_status is not None
            and final_status.get("state") == "stopped"
        )
        manifest.update(
            {
                "status": "completed" if success else "failed",
                "finished_at": M5.utc_now(),
                "keyboard_exit_code": keyboard_exit,
                "runtime_exit_code": runtime_exit,
                "controller_exit_code": controller_exit,
                "controller_final": final_status,
                "dashboard_health": dashboard_health,
            }
        )
        M5.atomic_write_json(manifest_path, manifest)
        timeline_mark(timeline_file, started_at, "interactive_cleanup_complete")
        timeline_file.with_suffix(timeline_file.suffix + ".lock").unlink(
            missing_ok=True
        )
        print("Manual-drive session stopped cleanly.", flush=True)
        return success
    except (
        OSError,
        RuntimeError,
        TimeoutError,
        subprocess.TimeoutExpired,
        InterruptedError,
    ) as error:
        manifest.update(
            {
                "status": "failed",
                "finished_at": M5.utc_now(),
                "error": str(error),
            }
        )
        M5.atomic_write_json(manifest_path, manifest)
        timeline_mark(timeline_file, started_at, "interactive_failed", error=str(error))
        print(f"M6.1 interactive error: {error}", file=sys.stderr, flush=True)
        return False
    finally:
        if keyboard is not None and keyboard.process.poll() is None:
            keyboard.stop()
        if dashboard is not None and dashboard.process.poll() is None:
            dashboard.stop()
        if runtime is not None and runtime.process.poll() is None:
            runtime.stop()
        if controller is not None and controller.process.poll() is None:
            controller.stop()
        log.close()
        print(f"M6.1 artifacts: {run_directory}", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--viss-client", required=True, type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--keyboard-ui", required=True, type=Path)
    parser.add_argument("--run-directory", required=True, type=Path)
    parser.add_argument("--started-timestamp", required=True, type=float)
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        config = CONTROLLER.load_config(arguments.config)
        if config["controller"]["type"] != "external_control":
            raise ValueError("M6.1 requires controller.type=external_control")
        for path in (
            arguments.config,
            arguments.runtime,
            arguments.viss_client,
            arguments.python,
            arguments.python_api_root,
            arguments.certificate,
            arguments.private_key,
            arguments.keyboard_ui,
        ):
            if not path.exists():
                raise ValueError(f"required path does not exist: {path}")
        return 0 if run(arguments, config) else 2
    except (OSError, ValueError) as error:
        print(f"M6.1 configuration error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
