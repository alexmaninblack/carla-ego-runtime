#!/usr/bin/env python3
"""Exercise scripted/manual handover against a running hybrid controller."""

# Copyright (c) 2026 maninblack
# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

from external_control_client import ControlConnection


def emit(event: str, **fields: Any) -> None:
    print(
        json.dumps(
            {"source": "hybrid_brake_acceptance", "event": event, **fields},
            sort_keys=True,
            separators=(",", ":"),
        ),
        flush=True,
    )


def heartbeat_for(connection: ControlConnection, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        connection.heartbeat()
        time.sleep(0.1)


def wait_for_scenario_completion(
    connection: ControlConnection, timeout_seconds: float
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        heartbeat = connection.heartbeat()
        if heartbeat.get("mode") == "safe_stop":
            reason = str(heartbeat.get("reason", ""))
            if reason != "scenario_complete":
                raise RuntimeError(f"scenario stopped with reason: {reason}")
            emit("scenario_completed")
            return
        time.sleep(0.1)
    raise TimeoutError("timed out waiting for scenario completion")


def manual_brake(connection: ControlConnection) -> None:
    connection.set_mode("manual")
    deadline = time.monotonic() + 0.8
    while time.monotonic() < deadline:
        connection.command(0.0, 0.65, 0.0)
        time.sleep(0.05)
    connection.set_mode("safe_stop")
    emit("manual_brake_completed")


def run(arguments: argparse.Namespace) -> None:
    connection = ControlConnection(
        arguments.socket,
        arguments.token,
        "hybrid-brake-acceptance",
        protocol_version=3,
    )
    try:
        required = {"safe_stop", "manual", "scenario"}
        if not required.issubset(connection.available_modes):
            raise RuntimeError(
                f"hybrid modes are unavailable: {connection.available_modes}"
            )
        connection.set_mode("scenario")
        heartbeat_for(connection, 2.0)
        manual_brake(connection)
        emit("scripted_run_aborted_by_manual_handover")

        connection.set_mode("scenario")
        wait_for_scenario_completion(connection, arguments.scenario_timeout_seconds)
        manual_brake(connection)
        connection.release()
    finally:
        connection.close()


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, type=Path)
    parser.add_argument("--token", required=True, type=Path)
    parser.add_argument("--scenario-timeout-seconds", type=float, default=45.0)
    return parser.parse_args()


def main() -> int:
    try:
        arguments = parse_arguments()
        if arguments.scenario_timeout_seconds < 10:
            raise ValueError("--scenario-timeout-seconds must be at least 10")
        run(arguments)
        emit("acceptance_passed")
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        emit("acceptance_failed", error=str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
