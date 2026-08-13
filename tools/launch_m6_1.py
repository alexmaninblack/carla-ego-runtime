#!/usr/bin/env python3
"""Cold/warm launcher for a CARLA M6.2 live-handover session."""

from __future__ import annotations

import argparse
import fcntl
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
from typing import Any, Dict, Optional, TextIO


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


TOOLS = Path(__file__).resolve().parent
BASE = load_module("m61_launch_base", TOOLS / "launch_m5.py")
M5 = load_module("m61_launch_json", TOOLS / "run_m5.py")
STOP_REQUESTED = False


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def timeline_mark(path: Path, started_at: float, stage: str, **fields: Any) -> None:
    lock_path = path.with_suffix(path.suffix + ".lock")
    with lock_path.open("a", encoding="utf-8") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        try:
            try:
                payload = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                payload = {"schema_version": 1, "stages": []}
            payload.setdefault("stages", []).append(
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


def orchestrator_command(
    arguments: argparse.Namespace,
    run_directory: Path,
    control_directory: Path,
    started_at: float,
) -> list[str]:
    return [
        str(arguments.python),
        str(TOOLS / "run_m6_interactive.py"),
        "--config",
        str(arguments.config),
        "--runtime",
        str(arguments.runtime),
        "--viss-client",
        str(arguments.viss_client),
        "--python",
        str(arguments.python),
        "--python-api-root",
        str(arguments.python_api_root),
        "--certificate",
        str(arguments.certificate),
        "--private-key",
        str(arguments.private_key),
        "--keyboard-ui",
        str(arguments.keyboard_ui),
        "--run-directory",
        str(run_directory),
        "--control-directory",
        str(control_directory),
        "--started-timestamp",
        str(started_at),
    ]


def validate_paths(arguments: argparse.Namespace) -> None:
    for option, path in {
        "--config": arguments.config,
        "--runtime": arguments.runtime,
        "--viss-client": arguments.viss_client,
        "--python": arguments.python,
        "--python-api-root": arguments.python_api_root,
        "--certificate": arguments.certificate,
        "--private-key": arguments.private_key,
        "--keyboard-source": arguments.keyboard_source,
        "--keyboard-info": arguments.keyboard_info,
    }.items():
        if not path.exists():
            raise ValueError(f"{option} does not exist: {path}")
    if arguments.carla_startup_timeout_seconds < 10:
        raise ValueError("--carla-startup-timeout-seconds must be at least 10")


def build_keyboard_app(arguments: argparse.Namespace) -> Path:
    app = arguments.keyboard_app
    executable = app / "Contents" / "MacOS" / "KeyboardControl"
    info = app / "Contents" / "Info.plist"
    source_mtime = max(
        arguments.keyboard_source.stat().st_mtime,
        arguments.keyboard_info.stat().st_mtime,
    )
    if (
        executable.exists()
        and info.exists()
        and min(executable.stat().st_mtime, info.stat().st_mtime) >= source_mtime
    ):
        return executable
    executable.parent.mkdir(parents=True, exist_ok=True)
    compile_command = [
        "xcrun",
        "swiftc",
        "-O",
        str(arguments.keyboard_source),
        "-o",
        str(executable),
        "-framework",
        "AppKit",
    ]
    environment = dict(os.environ)
    cache_root = arguments.run_root / ".swift-cache"
    cache_root.mkdir(parents=True, exist_ok=True)
    environment["CLANG_MODULE_CACHE_PATH"] = str(cache_root / "clang")
    environment["SWIFT_MODULECACHE_PATH"] = str(cache_root / "swift")
    result = subprocess.run(compile_command, env=environment, check=False)
    if result.returncode != 0:
        raise RuntimeError("native keyboard-control build failed")
    shutil.copy2(arguments.keyboard_info, info)
    result = subprocess.run(
        ["codesign", "--force", "--deep", "--sign", "-", str(app)], check=False
    )
    if result.returncode != 0:
        raise RuntimeError("native keyboard-control signing failed")
    return executable


def manual_run_cleanup_is_valid(
    run_directory: Path, control_directory: Path
) -> bool:
    status = M5.read_json(run_directory / "controller-status.json")
    if status is None or status.get("state") != "stopped":
        return False
    control = status.get("control", {})
    return (
        not control.get("session_active", True)
        and control.get("owner") is None
        and not (control_directory / "control.sock").exists()
        and not (control_directory / "control.token").exists()
    )


def run(arguments: argparse.Namespace) -> int:
    started_at = time.time()
    validate_paths(arguments)
    config: Dict[str, Any] = BASE.load_config(arguments.config)
    arguments.run_root.mkdir(parents=True, exist_ok=True)
    run_id = M5.utc_now().replace(":", "").replace("-", "")
    run_id += "-" + uuid.uuid4().hex[:8]
    run_directory = arguments.run_root / run_id
    run_directory.mkdir(parents=True, exist_ok=False)
    timeline_file = run_directory / "startup-timeline.json"
    M5.atomic_write_json(
        timeline_file,
        {
            "schema_version": 1,
            "run_id": run_id,
            "started_at": M5.utc_now(),
            "stages": [],
        },
    )
    timeline_mark(timeline_file, started_at, "launcher_started")
    arguments.keyboard_ui = build_keyboard_app(arguments)
    timeline_mark(timeline_file, started_at, "keyboard_app_ready")
    control_directory = Path(tempfile.mkdtemp(prefix="carla-m6-"))
    control_directory.chmod(0o700)

    lock = BASE.SessionLock(arguments.run_root / ".m6_2-session.lock")
    simulator: Optional[subprocess.Popen[str]] = None
    simulator_log: Optional[TextIO] = None
    orchestrator: Optional[subprocess.Popen[str]] = None
    try:
        lock.acquire()
        print("\033]0;CARLA M6.2 — VSS Dashboard\007", end="", flush=True)
        print("[1/5] Preflight: checking CARLA and local components...", flush=True)
        carla = BASE.import_carla(arguments.python_api_root)
        carla_config = config["carla"]
        map_name = BASE.probe_carla(
            carla, str(carla_config["host"]), int(carla_config["port"])
        )
        timeline_mark(
            timeline_file,
            started_at,
            "preflight_complete",
            carla_already_running=map_name is not None,
        )
        if map_name is None:
            print("[2/5] Cold start: opening Unreal and Town10HD...", flush=True)
            if arguments.unreal_log is None:
                arguments.unreal_log = run_directory / "unreal.log"
            simulator, simulator_log = BASE.start_simulator(arguments)
            timeline_mark(
                timeline_file,
                started_at,
                "unreal_process_started",
                pid=simulator.pid,
            )
            map_name = BASE.wait_for_carla(
                carla,
                config,
                arguments.carla_startup_timeout_seconds,
                simulator,
            )
            launch_mode = "cold"
        else:
            print("[2/5] Warm start: safely reusing the running CARLA world.", flush=True)
            launch_mode = "warm"
        expected_map = str(carla_config["expected_map"])
        if map_name != expected_map:
            raise RuntimeError(f"expected map {expected_map}, got {map_name}")
        timeline_mark(
            timeline_file,
            started_at,
            "carla_rpc_ready",
            launch_mode=launch_mode,
            map=map_name,
        )
        print(f"[3/5] CARLA RPC ready on {map_name}.", flush=True)
        print("[4/5] Starting vehicle, telemetry, dashboard, and keyboard...", flush=True)
        orchestrator = subprocess.Popen(
            orchestrator_command(
                arguments,
                run_directory,
                control_directory,
                started_at,
            ),
            text=True,
            start_new_session=True,
        )
        timeline_mark(timeline_file, started_at, "orchestrator_started")
        print("[5/5] Watch this terminal; it will become the live VSS dashboard.", flush=True)
        while orchestrator.poll() is None:
            if STOP_REQUESTED:
                orchestrator.send_signal(signal.SIGINT)
            time.sleep(0.1)
        timeline_mark(
            timeline_file,
            started_at,
            "orchestrator_finished",
            exit_code=int(orchestrator.returncode),
        )
        if orchestrator.returncode == 0 and not manual_run_cleanup_is_valid(
            run_directory, control_directory
        ):
            raise RuntimeError("live-handover cleanup verification failed")
        if orchestrator.returncode == 0:
            timeline_mark(timeline_file, started_at, "run_cleanup_verified")
        return int(orchestrator.returncode)
    finally:
        if orchestrator is not None and orchestrator.poll() is None:
            orchestrator.send_signal(signal.SIGINT)
            try:
                orchestrator.wait(timeout=45)
            except subprocess.TimeoutExpired:
                orchestrator.terminate()
                orchestrator.wait(timeout=10)
        if simulator is not None and not arguments.keep_owned_simulator_running:
            BASE.stop_owned_process(simulator, "CARLA simulator")
            timeline_mark(timeline_file, started_at, "owned_simulator_stopped")
        if simulator_log is not None:
            simulator_log.close()
        lock.release()
        shutil.rmtree(control_directory, ignore_errors=True)
        timeline_mark(timeline_file, started_at, "launcher_cleanup_complete")
        timeline_file.with_suffix(timeline_file.suffix + ".lock").unlink(
            missing_ok=True
        )
        print(f"Startup timeline: {timeline_file}", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--viss-client", required=True, type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--keyboard-source", required=True, type=Path)
    parser.add_argument("--keyboard-info", required=True, type=Path)
    parser.add_argument("--keyboard-app", required=True, type=Path)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--unreal-editor", type=Path)
    parser.add_argument("--uproject", type=Path)
    parser.add_argument("--startup-map")
    parser.add_argument("--unreal-argument", action="append", default=[])
    parser.add_argument("--unreal-log", type=Path)
    parser.add_argument("--carla-startup-timeout-seconds", type=float, default=300)
    parser.add_argument("--keep-owned-simulator-running", action="store_true")
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        return run(arguments)
    except InterruptedError:
        print("M6.2 launcher stopped cleanly by the operator.", flush=True)
        return 130
    except (OSError, RuntimeError, TimeoutError, ValueError, json.JSONDecodeError) as error:
        print(f"M6.2 launcher error: {error}", file=sys.stderr, flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
