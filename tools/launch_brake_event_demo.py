#!/usr/bin/env python3
"""Start CARLA and run the deterministic brake-event demonstration."""

# Copyright (c) 2026 maninblack
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional, TextIO


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


TOOLS = Path(__file__).resolve().parent
BASE = load_module("brake_demo_launcher_base", TOOLS / "launch_m5.py")
STOP_REQUESTED = False


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def run_id() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")


def orchestrator_command(arguments: argparse.Namespace, directory: Path) -> list[str]:
    command = [
        str(arguments.python),
        str(TOOLS / "run_brake_event_demo.py"),
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
        "--run-directory",
        str(directory),
        "--display-hold-seconds",
        str(arguments.display_hold_seconds),
    ]
    if arguments.dashboard_quiet:
        command.append("--dashboard-quiet")
    return command


def validate_paths(arguments: argparse.Namespace) -> None:
    for option, path in {
        "--config": arguments.config,
        "--runtime": arguments.runtime,
        "--viss-client": arguments.viss_client,
        "--python": arguments.python,
        "--python-api-root": arguments.python_api_root,
        "--certificate": arguments.certificate,
        "--private-key": arguments.private_key,
    }.items():
        if not path.exists():
            raise ValueError(f"{option} does not exist: {path}")
    if arguments.carla_startup_timeout_seconds < 10:
        raise ValueError("--carla-startup-timeout-seconds must be at least 10")


def run(arguments: argparse.Namespace) -> int:
    validate_paths(arguments)
    config = BASE.load_config(arguments.config)
    lock = BASE.SessionLock(arguments.run_root / ".brake-demo-session.lock")
    simulator: Optional[subprocess.Popen[str]] = None
    simulator_log: Optional[TextIO] = None
    orchestrator: Optional[subprocess.Popen[str]] = None
    try:
        lock.acquire()
        print("[1/4] Checking the CARLA simulator...", flush=True)
        carla = BASE.import_carla(arguments.python_api_root)
        carla_config = config["carla"]
        map_name = BASE.probe_carla(
            carla, str(carla_config["host"]), int(carla_config["port"])
        )
        if map_name is None:
            print("[2/4] Starting Unreal and loading Town10HD...", flush=True)
            simulator, simulator_log = BASE.start_simulator(arguments)
            map_name = BASE.wait_for_carla(
                carla,
                config,
                arguments.carla_startup_timeout_seconds,
                simulator,
            )
        else:
            print("[2/4] CARLA is already running; reusing it safely.", flush=True)
        expected_map = str(carla_config["expected_map"])
        if map_name != expected_map:
            raise RuntimeError(f"expected map {expected_map}, got {map_name}")
        print(f"[3/4] CARLA is ready on {map_name}.", flush=True)
        run_directory = arguments.run_root / run_id()
        print("[4/4] Starting the brake event and engineering dashboard...", flush=True)
        orchestrator = subprocess.Popen(
            orchestrator_command(arguments, run_directory),
            text=True,
            start_new_session=True,
        )
        while orchestrator.poll() is None:
            if STOP_REQUESTED:
                orchestrator.send_signal(signal.SIGINT)
            time.sleep(0.1)
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
        if simulator_log is not None:
            simulator_log.close()
        lock.release()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--viss-client", required=True, type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-api-root", required=True, type=Path)
    parser.add_argument("--certificate", required=True, type=Path)
    parser.add_argument("--private-key", required=True, type=Path)
    parser.add_argument("--run-root", type=Path, default=Path("runs/brake-event"))
    parser.add_argument("--unreal-editor", type=Path)
    parser.add_argument("--uproject", type=Path)
    parser.add_argument("--startup-map")
    parser.add_argument("--unreal-argument", action="append", default=[])
    parser.add_argument("--unreal-log", type=Path)
    parser.add_argument("--carla-startup-timeout-seconds", type=float, default=240)
    parser.add_argument("--display-hold-seconds", type=float, default=3.0)
    parser.add_argument("--dashboard-quiet", action="store_true")
    parser.add_argument("--keep-owned-simulator-running", action="store_true")
    return parser.parse_args()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    try:
        return run(parse_arguments())
    except InterruptedError:
        print("Brake-event launcher stopped cleanly by the operator.", flush=True)
        return 130
    except (
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        TimeoutError,
        ValueError,
    ) as error:
        print(f"Brake-event launcher error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
