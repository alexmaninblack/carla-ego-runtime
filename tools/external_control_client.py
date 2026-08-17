#!/usr/bin/env python3
"""Independent M6 client for scripted control and safety acceptance."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict


TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS.parent))
from tools.external_control_protocol import request  # noqa: E402


def emit(event: str, **fields: Any) -> None:
    print(
        json.dumps(
            {"source": "external_control_client", "event": event, **fields},
            sort_keys=True,
            separators=(",", ":"),
        ),
        flush=True,
    )


def require_ok(response: Dict[str, Any]) -> Dict[str, Any]:
    if response.get("status") != "ok":
        raise RuntimeError(f"control request failed: {response}")
    return response


class ControlConnection:
    def __init__(
        self,
        socket_path: Path,
        token_path: Path,
        client_id: str,
        protocol_version: int = 1,
    ):
        if protocol_version not in (1, 2, 3):
            raise ValueError("protocol_version must be 1, 2, or 3")
        self.protocol_version = protocol_version
        self.connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.connection.settimeout(2.0)
        self.connection.connect(str(socket_path))
        token = token_path.read_text(encoding="utf-8").strip()
        acquired = require_ok(
            request(
                self.connection,
                {
                    "version": self.protocol_version,
                    "action": "acquire",
                    "requestId": "acquire-1",
                    "clientId": client_id,
                    "token": token,
                },
            )
        )
        self.session_id = str(acquired["sessionId"])
        self.available_modes = [
            str(mode) for mode in acquired.get("availableModes", [])
        ]
        self.sequence = 0
        self.request_sequence = 1
        emit("control_acquired", client_id=client_id)

    def _request_id(self, prefix: str) -> str:
        self.request_sequence += 1
        return f"{prefix}-{self.request_sequence}"

    def command(self, throttle: float, brake: float, steering: float) -> None:
        self.sequence += 1
        require_ok(
            request(
                self.connection,
                {
                    "version": self.protocol_version,
                    "action": "command",
                    "requestId": self._request_id("command"),
                    "sessionId": self.session_id,
                    "sequence": self.sequence,
                    "throttle": throttle,
                    "brake": brake,
                    "steering": steering,
                },
            )
        )

    def heartbeat(self) -> Dict[str, Any]:
        return require_ok(
            request(
                self.connection,
                {
                    "version": self.protocol_version,
                    "action": "heartbeat",
                    "requestId": self._request_id("heartbeat"),
                    "sessionId": self.session_id,
                },
            )
        )

    def set_mode(self, mode: str) -> None:
        if self.protocol_version < 2:
            raise RuntimeError("set_mode requires protocol version 2 or 3")
        require_ok(
            request(
                self.connection,
                {
                    "version": self.protocol_version,
                    "action": "set_mode",
                    "requestId": self._request_id("mode"),
                    "sessionId": self.session_id,
                    "mode": mode,
                },
            )
        )
        emit("drive_mode_changed", mode=mode)

    def release(self) -> None:
        require_ok(
            request(
                self.connection,
                {
                    "version": self.protocol_version,
                    "action": "release",
                    "requestId": self._request_id("release"),
                    "sessionId": self.session_id,
                },
            )
        )
        emit("control_released", sequence=self.sequence)

    def close(self) -> None:
        self.connection.close()


def send_for(
    control: ControlConnection,
    duration: float,
    rate_hz: float,
    throttle: float,
    brake: float,
    steering: float,
) -> None:
    deadline = time.monotonic() + duration
    period = 1.0 / rate_hz
    next_command = time.monotonic()
    while time.monotonic() < deadline:
        control.command(throttle, brake, steering)
        next_command += period
        remaining = next_command - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)


def heartbeat_for(control: ControlConnection, duration: float, rate_hz: float) -> None:
    deadline = time.monotonic() + duration
    period = 1.0 / rate_hz
    next_heartbeat = time.monotonic()
    while time.monotonic() < deadline:
        control.heartbeat()
        next_heartbeat += period
        remaining = next_heartbeat - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)


def run_acceptance(arguments: argparse.Namespace) -> None:
    control = ControlConnection(
        arguments.socket, arguments.token, arguments.client_id
    )
    try:
        emit("phase_started", phase="straight")
        send_for(control, 3.0, arguments.command_rate_hz, 0.35, 0.0, 0.0)
        emit("phase_started", phase="right_turn")
        send_for(control, 2.0, arguments.command_rate_hz, 0.25, 0.0, 0.22)
        emit("phase_started", phase="command_timeout")
        heartbeat_for(control, 0.6, 10.0)
        emit("phase_started", phase="recovery")
        send_for(control, 1.5, arguments.command_rate_hz, 0.20, 0.0, -0.10)
        emit("phase_started", phase="brake")
        send_for(control, 1.0, arguments.command_rate_hz, 0.0, 0.45, 0.0)
        control.release()
    finally:
        control.close()


def run_disconnect(arguments: argparse.Namespace) -> None:
    control = ControlConnection(
        arguments.socket, arguments.token, arguments.client_id
    )
    emit("phase_started", phase="disconnect")
    send_for(control, 1.5, arguments.command_rate_hz, 0.25, 0.0, 0.0)
    control.close()
    emit("control_disconnected_intentionally", sequence=control.sequence)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, type=Path)
    parser.add_argument("--token", required=True, type=Path)
    parser.add_argument("--client-id", default="m6-acceptance-client")
    parser.add_argument("--command-rate-hz", type=float, default=20.0)
    parser.add_argument(
        "--scenario", choices=("acceptance", "disconnect"), default="acceptance"
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.command_rate_hz < 5 or arguments.command_rate_hz > 100:
            raise ValueError("--command-rate-hz must be between 5 and 100")
        if arguments.scenario == "acceptance":
            run_acceptance(arguments)
        else:
            run_disconnect(arguments)
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        emit("client_failed", error=str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
