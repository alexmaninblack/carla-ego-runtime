#!/usr/bin/env python3
"""Bridge newline JSON controls from the native M6.1 UI to the M6 socket."""

from __future__ import annotations

import argparse
import json
import selectors
import sys
import time
from pathlib import Path


TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS.parent))
from tools.external_control_client import ControlConnection, ControlRequestRejected  # noqa: E402


def manual_command(connection, value):
    """A rejected input must stop motion, not permanently strand the UI."""
    try:
        connection.command(float(value["throttle"]), float(value["brake"]), float(value["steering"]))
        return True
    except ControlRequestRejected as error:
        if error.code not in ("invalid_command", "invalid_mode", "invalid_sequence", "orchestration_busy"):
            raise
        connection.set_mode("safe_stop")
        emit("mode_changed", mode="safe_stop", reason="command_rejected")
        return False


def emit(event: str, **fields: object) -> None:
    print(
        json.dumps(
            {"source": "keyboard_control_bridge", "event": event, **fields},
            sort_keys=True,
            separators=(",", ":"),
        ),
        flush=True,
    )


def observed_mode(current_mode, heartbeat):
    # Physical control stays stopped during the bounded first-command wait.
    # Do not cancel the selected Manual mode before the UI can send that
    # command. A real timeout, hold or operator stop still wins immediately.
    if (current_mode == "manual" and heartbeat.get("reason") == "awaiting_command"
            and heartbeat.get("held") is not True):
        return current_mode
    return str(heartbeat.get("mode", current_mode))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, type=Path)
    parser.add_argument("--token", required=True, type=Path)
    parser.add_argument("--client-id", default="m6-keyboard-client")
    arguments = parser.parse_args()
    connection = None
    try:
        connection = ControlConnection(
            arguments.socket,
            arguments.token,
            arguments.client_id,
            protocol_version=3,
        )
        emit("bridge_ready", available_modes=connection.available_modes)
        selector = selectors.DefaultSelector()
        selector.register(sys.stdin, selectors.EVENT_READ)
        next_heartbeat = time.monotonic() + 0.2
        exiting = False
        current_mode = "safe_stop"
        orchestration_held = False
        while not exiting:
            timeout = max(0.0, next_heartbeat - time.monotonic())
            events = selector.select(timeout)
            if events:
                line = sys.stdin.readline()
                if not line:
                    break
                value = json.loads(line)
                action = value.get("action", "command")
                if action == "set_mode":
                    mode = str(value["mode"])
                    try:
                        connection.set_mode(mode)
                        current_mode = mode
                        emit("mode_changed", mode=mode)
                    except ControlRequestRejected as error:
                        emit("mode_rejected", mode=mode, error=str(error))
                elif action == "exit":
                    connection.set_mode("safe_stop")
                    current_mode = "safe_stop"
                    exiting = True
                elif current_mode == "manual" and not orchestration_held:
                    if not manual_command(connection, value):
                        current_mode = "safe_stop"
            if time.monotonic() >= next_heartbeat and not exiting:
                heartbeat = connection.heartbeat()
                orchestration_held = heartbeat.get("held") is True
                server_mode = observed_mode(current_mode, heartbeat)
                if server_mode != current_mode:
                    current_mode = server_mode
                    emit(
                        "mode_changed",
                        mode=server_mode,
                        reason=str(heartbeat.get("reason", "")),
                    )
                next_heartbeat = time.monotonic() + 0.2
        try:
            connection.set_mode("safe_stop")
        except RuntimeError:
            pass
        connection.release()
        return 0
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as error:
        emit("bridge_failed", error=str(error))
        return 2
    finally:
        if connection is not None:
            connection.close()


if __name__ == "__main__":
    raise SystemExit(main())
