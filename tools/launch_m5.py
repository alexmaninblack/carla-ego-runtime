#!/usr/bin/env python3
"""Launch a complete operator-facing M5 session with visible progress."""

from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, TextIO, Tuple


STOP_REQUESTED = False


def request_stop(_signum: int, _frame: Any) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def load_config(path: Path) -> Dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("configuration root must be an object")
    return value


class SessionLock:
    """Prevent two operator launchers from owning the same local session."""

    def __init__(self, path: Path):
        self.path = path
        self.acquired = False

    @staticmethod
    def _is_live(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def acquire(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        for attempt in range(2):
            try:
                descriptor = os.open(
                    self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600
                )
            except FileExistsError:
                try:
                    owner = int(self.path.read_text(encoding="utf-8").strip())
                except (OSError, ValueError):
                    owner = -1
                if owner > 0 and self._is_live(owner):
                    raise RuntimeError(
                        f"another M5 launcher is already running (pid {owner})"
                    )
                if attempt == 0:
                    self.path.unlink(missing_ok=True)
                    continue
                raise RuntimeError("could not replace a stale M5 session lock")
            else:
                with os.fdopen(descriptor, "w", encoding="utf-8") as lock_file:
                    lock_file.write(f"{os.getpid()}\n")
                self.acquired = True
                return

    def release(self) -> None:
        if self.acquired:
            self.path.unlink(missing_ok=True)
            self.acquired = False


def import_carla(python_api_root: Path) -> Any:
    sys.path.insert(0, str(python_api_root))
    try:
        import carla  # type: ignore
    except ImportError as error:
        raise RuntimeError(
            "the matching CARLA Python API is not available"
        ) from error
    return carla


def probe_carla(carla: Any, host: str, port: int) -> Optional[str]:
    try:
        client = carla.Client(host, port)
        client.set_timeout(2.0)
        return str(client.get_world().get_map().name)
    except RuntimeError:
        return None


def simulator_command(arguments: argparse.Namespace) -> List[str]:
    missing = [
        option
        for option, value in {
            "--unreal-editor": arguments.unreal_editor,
            "--uproject": arguments.uproject,
            "--startup-map": arguments.startup_map,
        }.items()
        if value is None
    ]
    if missing:
        raise ValueError(
            "CARLA is not running and simulator launch options are missing: "
            + ", ".join(missing)
        )
    if not arguments.unreal_editor.is_file():
        raise ValueError(f"Unreal Editor is unavailable: {arguments.unreal_editor}")
    if not arguments.uproject.is_file():
        raise ValueError(f"CARLA project is unavailable: {arguments.uproject}")
    return [
        str(arguments.unreal_editor),
        str(arguments.uproject),
        str(arguments.startup_map),
        *arguments.unreal_argument,
    ]


def start_simulator(
    arguments: argparse.Namespace,
) -> Tuple[subprocess.Popen[str], Optional[TextIO]]:
    command = simulator_command(arguments)
    log_file: Optional[TextIO] = None
    output: Any = subprocess.DEVNULL
    if arguments.unreal_log is not None:
        arguments.unreal_log.parent.mkdir(parents=True, exist_ok=True)
        log_file = arguments.unreal_log.open("a", encoding="utf-8")
        output = log_file
    process = subprocess.Popen(
        command,
        stdout=output,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    return process, log_file


def wait_for_carla(
    carla: Any,
    config: Dict[str, Any],
    timeout_seconds: float,
    simulator: Optional[subprocess.Popen[str]],
) -> str:
    carla_config = config["carla"]
    deadline = time.monotonic() + timeout_seconds
    next_update = 0.0
    while time.monotonic() < deadline:
        if STOP_REQUESTED:
            raise InterruptedError("operator requested stop during startup")
        if simulator is not None and simulator.poll() is not None:
            raise RuntimeError(
                f"Unreal Editor exited during startup with code {simulator.returncode}"
            )
        map_name = probe_carla(
            carla, str(carla_config["host"]), int(carla_config["port"])
        )
        if map_name is not None:
            return map_name
        now = time.monotonic()
        if now >= next_update:
            remaining = max(0, int(deadline - now))
            print(
                "      Loading Unreal, the city, and CARLA services "
                f"({remaining}s timeout remaining)...",
                flush=True,
            )
            next_update = now + 5.0
        time.sleep(0.25)
    raise TimeoutError("CARLA did not become ready before the startup timeout")


def orchestrator_command(arguments: argparse.Namespace) -> List[str]:
    command = [
        str(arguments.python),
        str(Path(__file__).with_name("run_m5.py")),
        "--config", str(arguments.config),
        "--runtime", str(arguments.runtime),
        "--viss-client", str(arguments.viss_client),
        "--python", str(arguments.python),
        "--python-api-root", str(arguments.python_api_root),
        "--certificate", str(arguments.certificate),
        "--private-key", str(arguments.private_key),
        "--run-root", str(arguments.run_root),
        "--dashboard",
    ]
    if arguments.route_cycles is not None:
        command.extend(["--route-cycles", str(arguments.route_cycles)])
    if arguments.maximum_route_seconds is not None:
        command.extend(
            ["--maximum-route-seconds", str(arguments.maximum_route_seconds)]
        )
    if arguments.dashboard_quiet:
        command.append("--dashboard-quiet")
    return command


def stop_owned_process(process: subprocess.Popen[str], name: str) -> None:
    if process.poll() is not None:
        return
    print(f"Stopping launcher-owned {name}...", flush=True)
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


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
    parser.add_argument("--unreal-editor", type=Path)
    parser.add_argument("--uproject", type=Path)
    parser.add_argument("--startup-map")
    parser.add_argument("--unreal-argument", action="append", default=[])
    parser.add_argument("--unreal-log", type=Path)
    parser.add_argument("--carla-startup-timeout-seconds", type=float, default=240)
    parser.add_argument("--route-cycles", type=int)
    parser.add_argument("--maximum-route-seconds", type=float)
    parser.add_argument("--dashboard-quiet", action="store_true")
    parser.add_argument("--keep-owned-simulator-running", action="store_true")
    return parser.parse_args()


def run(arguments: argparse.Namespace) -> int:
    validate_paths(arguments)
    config = load_config(arguments.config)
    lock = SessionLock(arguments.run_root / ".m5-session.lock")
    simulator: Optional[subprocess.Popen[str]] = None
    simulator_log: Optional[TextIO] = None
    orchestrator: Optional[subprocess.Popen[str]] = None
    try:
        lock.acquire()
        print("[1/4] Checking CARLA simulator...", flush=True)
        carla = import_carla(arguments.python_api_root)
        carla_config = config["carla"]
        map_name = probe_carla(
            carla, str(carla_config["host"]), int(carla_config["port"])
        )
        if map_name is None:
            print("[2/4] Starting Unreal Editor and loading the city...", flush=True)
            simulator, simulator_log = start_simulator(arguments)
            map_name = wait_for_carla(
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
        print("[4/4] Starting the vehicle, VSS service, and dashboard...", flush=True)
        orchestrator = subprocess.Popen(
            orchestrator_command(arguments), text=True, start_new_session=True
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
        if (
            simulator is not None
            and not arguments.keep_owned_simulator_running
        ):
            stop_owned_process(simulator, "CARLA simulator")
        if simulator_log is not None:
            simulator_log.close()
        lock.release()


def main() -> int:
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    arguments = parse_arguments()
    try:
        return run(arguments)
    except InterruptedError:
        print("M5 launcher stopped cleanly by the operator.", flush=True)
        return 130
    except (OSError, RuntimeError, TimeoutError, ValueError, json.JSONDecodeError) as error:
        print(f"M5 launcher error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
