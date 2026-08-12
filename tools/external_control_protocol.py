#!/usr/bin/env python3
"""Thread-safe M6 external-control contract and local JSON-lines server."""

from __future__ import annotations

import hmac
import datetime as dt
import json
import math
import os
import secrets
import socket
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, Optional, Tuple


CONTRACT_VERSION = 2
SUPPORTED_VERSIONS = {1, 2}
DRIVE_MODES = {"safe_stop", "manual", "autopilot"}
MAX_MESSAGE_BYTES = 16 * 1024
SAFE_CONTROL = {"throttle": 0.0, "brake": 1.0, "steering": 0.0}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


class ControlProtocolError(ValueError):
    def __init__(self, code: str, description: str):
        super().__init__(description)
        self.code = code
        self.description = description


@dataclass(frozen=True)
class AppliedControl:
    throttle: float
    brake: float
    steering: float
    sequence: int
    safe_stop: bool
    reason: str
    mode: str


def _number(value: Any, name: str, minimum: float, maximum: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ControlProtocolError("invalid_command", f"{name} must be a number")
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < minimum or parsed > maximum:
        raise ControlProtocolError(
            "invalid_command", f"{name} must be between {minimum} and {maximum}"
        )
    return parsed


def _string(message: Dict[str, Any], name: str) -> str:
    value = message.get(name)
    if not isinstance(value, str) or not value:
        raise ControlProtocolError("bad_request", f"{name} must be a string")
    return value


class ExternalControlState:
    def __init__(
        self,
        token: str,
        command_timeout_seconds: float,
        ownership_timeout_seconds: float,
        event_sink: Optional[Callable[[str, Dict[str, Any]], None]] = None,
        mode_validator: Optional[Callable[[str], Optional[str]]] = None,
    ):
        if not token:
            raise ValueError("token must not be empty")
        if command_timeout_seconds <= 0:
            raise ValueError("command timeout must be positive")
        if ownership_timeout_seconds <= command_timeout_seconds:
            raise ValueError("ownership timeout must exceed command timeout")
        self._token = token
        self._command_timeout = command_timeout_seconds
        self._ownership_timeout = ownership_timeout_seconds
        self._event_sink = event_sink
        self._mode_validator = mode_validator
        self._lock = threading.Lock()
        self._session_id: Optional[str] = None
        self._client_id: Optional[str] = None
        self._last_sequence = 0
        self._last_command_at: Optional[float] = None
        self._last_heartbeat_at: Optional[float] = None
        self._command = dict(SAFE_CONTROL)
        self._safe_stop_reason = "startup"
        self._mode = "safe_stop"
        self._metrics: Dict[str, int] = {
            "acquisitions": 0,
            "commands": 0,
            "heartbeats": 0,
            "releases": 0,
            "disconnects": 0,
            "command_timeouts": 0,
            "ownership_timeouts": 0,
            "rejected_messages": 0,
            "mode_changes": 0,
            "manual_activations": 0,
            "autopilot_activations": 0,
        }

    def _event(self, event: str, **fields: Any) -> None:
        if self._event_sink is not None:
            self._event_sink(event, fields)

    def _require_session(self, message: Dict[str, Any]) -> str:
        session_id = _string(message, "sessionId")
        if self._session_id is None or not hmac.compare_digest(
            session_id, self._session_id
        ):
            raise ControlProtocolError("invalid_session", "control session is invalid")
        return session_id

    def _select_safe_stop(self, reason: str) -> None:
        self._command = dict(SAFE_CONTROL)
        self._last_command_at = None
        if self._safe_stop_reason != reason:
            self._safe_stop_reason = reason
            self._event("safe_stop_selected", reason=reason)

    def _drop_ownership(self, reason: str) -> None:
        self._mode = "safe_stop"
        self._select_safe_stop(reason)
        self._session_id = None
        self._client_id = None
        self._last_sequence = 0
        self._last_heartbeat_at = None

    def handle(self, message: Dict[str, Any], now: float) -> Dict[str, Any]:
        with self._lock:
            try:
                return self._handle_locked(message, now)
            except ControlProtocolError:
                self._metrics["rejected_messages"] += 1
                raise

    def _handle_locked(
        self, message: Dict[str, Any], now: float
    ) -> Dict[str, Any]:
        version = message.get("version")
        if version not in SUPPORTED_VERSIONS:
            raise ControlProtocolError("bad_request", "version must be 1 or 2")
        action = _string(message, "action")
        _string(message, "requestId")

        if action == "acquire":
            supplied_token = _string(message, "token")
            client_id = _string(message, "clientId")
            if not hmac.compare_digest(supplied_token, self._token):
                raise ControlProtocolError("unauthorized", "control is unavailable")
            if self._session_id is not None:
                raise ControlProtocolError("control_busy", "control is unavailable")
            self._session_id = uuid.uuid4().hex
            self._client_id = client_id
            self._last_sequence = 0
            self._last_command_at = now
            self._last_heartbeat_at = now
            if version == 1:
                self._mode = "manual"
                self._safe_stop_reason = "awaiting_command"
            else:
                self._mode = "safe_stop"
                self._select_safe_stop("acquired")
            self._metrics["acquisitions"] += 1
            self._event("control_acquired", client_id=client_id)
            return {"status": "ok", "sessionId": self._session_id}

        if action == "command":
            self._require_session(message)
            if self._mode != "manual":
                raise ControlProtocolError(
                    "invalid_mode", "manual mode is required for commands"
                )
            sequence = message.get("sequence")
            if (
                isinstance(sequence, bool)
                or not isinstance(sequence, int)
                or sequence <= self._last_sequence
            ):
                raise ControlProtocolError(
                    "invalid_sequence", "sequence must strictly increase"
                )
            throttle = _number(message.get("throttle"), "throttle", 0.0, 1.0)
            brake = _number(message.get("brake"), "brake", 0.0, 1.0)
            steering = _number(message.get("steering"), "steering", -1.0, 1.0)
            if throttle > 0.0 and brake > 0.0:
                raise ControlProtocolError(
                    "invalid_command", "throttle and brake cannot both be non-zero"
                )
            self._last_sequence = sequence
            self._last_command_at = now
            self._last_heartbeat_at = now
            self._command = {
                "throttle": throttle,
                "brake": brake,
                "steering": steering,
            }
            self._safe_stop_reason = "command"
            self._metrics["commands"] += 1
            return {"status": "ok", "sequence": sequence}

        if action == "set_mode":
            if version != 2:
                raise ControlProtocolError(
                    "bad_request", "set_mode requires protocol version 2"
                )
            self._require_session(message)
            mode = _string(message, "mode")
            if mode not in DRIVE_MODES:
                raise ControlProtocolError(
                    "invalid_mode", "mode must be safe_stop, manual, or autopilot"
                )
            previous_mode = self._mode
            if mode == previous_mode:
                self._last_heartbeat_at = now
                return {"status": "ok", "mode": mode}
            if self._mode_validator is not None:
                unavailable = self._mode_validator(mode)
                if unavailable is not None:
                    raise ControlProtocolError("mode_unavailable", unavailable)
            self._mode = mode
            self._last_heartbeat_at = now
            if mode == "manual":
                self._select_safe_stop("awaiting_command")
                self._metrics["manual_activations"] += 1
            elif mode == "autopilot":
                self._command = dict(SAFE_CONTROL)
                self._last_command_at = None
                self._safe_stop_reason = "autopilot"
                self._metrics["autopilot_activations"] += 1
            else:
                self._select_safe_stop("operator_stop")
            self._metrics["mode_changes"] += 1
            self._event(
                "drive_mode_changed",
                previous_mode=previous_mode,
                mode=mode,
                client_id=self._client_id,
            )
            return {"status": "ok", "mode": mode}

        if action == "heartbeat":
            self._require_session(message)
            self._last_heartbeat_at = now
            self._metrics["heartbeats"] += 1
            return {"status": "ok"}

        if action == "release":
            self._require_session(message)
            self._metrics["releases"] += 1
            self._event("control_released", client_id=self._client_id)
            self._drop_ownership("release")
            return {"status": "ok"}

        raise ControlProtocolError("bad_request", "action is unsupported")

    def disconnect(self, session_id: Optional[str]) -> None:
        with self._lock:
            if (
                session_id is not None
                and self._session_id is not None
                and hmac.compare_digest(session_id, self._session_id)
            ):
                self._metrics["disconnects"] += 1
                self._event("control_disconnected", client_id=self._client_id)
                self._drop_ownership("disconnect")

    def current_control(self, now: float) -> AppliedControl:
        with self._lock:
            if self._session_id is not None and self._last_heartbeat_at is not None:
                if now - self._last_heartbeat_at > self._ownership_timeout:
                    self._metrics["ownership_timeouts"] += 1
                    self._drop_ownership("ownership_timeout")
                elif self._mode == "manual" and (
                    self._last_command_at is None
                    or now - self._last_command_at > self._command_timeout
                ):
                    if self._safe_stop_reason != "command_timeout":
                        self._metrics["command_timeouts"] += 1
                    self._select_safe_stop("command_timeout")
            return AppliedControl(
                throttle=float(self._command["throttle"]),
                brake=float(self._command["brake"]),
                steering=float(self._command["steering"]),
                sequence=self._last_sequence,
                safe_stop=(
                    self._mode == "safe_stop"
                    or (self._mode == "manual" and self._safe_stop_reason != "command")
                ),
                reason=self._safe_stop_reason,
                mode=self._mode,
            )

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "owner": self._client_id,
                "session_active": self._session_id is not None,
                "last_sequence": self._last_sequence,
                "safe_stop_reason": self._safe_stop_reason,
                "mode": self._mode,
                **self._metrics,
            }


def success_response(message: Dict[str, Any], result: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "version": message.get("version", CONTRACT_VERSION),
        "action": message.get("action", "unknown"),
        "requestId": message.get("requestId", ""),
        "ts": utc_now(),
        **result,
    }


def error_response(
    message: Dict[str, Any], error: ControlProtocolError
) -> Dict[str, Any]:
    return {
        "version": message.get("version", CONTRACT_VERSION),
        "action": message.get("action", "unknown"),
        "requestId": message.get("requestId", ""),
        "ts": utc_now(),
        "error": {"code": error.code, "description": error.description},
    }


class LocalControlServer:
    def __init__(
        self,
        socket_path: Path,
        token_path: Path,
        state: ExternalControlState,
        snapshot_sink: Optional[Callable[[Dict[str, Any]], None]] = None,
    ):
        self.socket_path = socket_path
        self.token_path = token_path
        self.state = state
        self.snapshot_sink = snapshot_sink
        self.ready = threading.Event()
        self._stop = threading.Event()
        self._listener: Optional[socket.socket] = None
        self._startup_error: Optional[BaseException] = None
        self._connection_threads: list[threading.Thread] = []
        self._connection_threads_lock = threading.Lock()
        self._connections: set[socket.socket] = set()
        self._connections_lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True)

    @staticmethod
    def create_token_file(path: Path) -> str:
        path.parent.mkdir(parents=True, exist_ok=True)
        token = secrets.token_urlsafe(32)
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as token_file:
            token_file.write(token + "\n")
        return token

    def start(self) -> None:
        self._thread.start()
        if not self.ready.wait(5):
            raise RuntimeError("local control server did not become ready")
        if self._startup_error is not None:
            raise RuntimeError(
                f"local control server failed to start: {self._startup_error}"
            ) from self._startup_error

    def _publish_snapshot(self) -> None:
        if self.snapshot_sink is not None:
            self.snapshot_sink(self.state.snapshot())

    def _run(self) -> None:
        self.socket_path.parent.mkdir(parents=True, exist_ok=True)
        self.socket_path.unlink(missing_ok=True)
        listener: Optional[socket.socket] = None
        try:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._listener = listener
            listener.bind(str(self.socket_path))
            os.chmod(self.socket_path, 0o600)
            listener.listen(4)
            listener.settimeout(0.1)
            self.ready.set()
            self._publish_snapshot()
            while not self._stop.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                connection_thread = threading.Thread(
                    target=self._serve_connection,
                    args=(connection,),
                    daemon=True,
                )
                with self._connection_threads_lock:
                    self._connection_threads.append(connection_thread)
                connection_thread.start()
        except BaseException as error:
            self._startup_error = error
            self.ready.set()
        finally:
            if listener is not None:
                listener.close()

    def _serve_connection(self, connection: socket.socket) -> None:
        with self._connections_lock:
            self._connections.add(connection)
        connection.settimeout(0.1)
        buffer = b""
        connection_session: Optional[str] = None
        try:
            while not self._stop.is_set():
                try:
                    chunk = connection.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                buffer += chunk
                if len(buffer) > MAX_MESSAGE_BYTES and b"\n" not in buffer:
                    break
                while b"\n" in buffer:
                    raw, buffer = buffer.split(b"\n", 1)
                    if not raw or len(raw) > MAX_MESSAGE_BYTES:
                        continue
                    message: Dict[str, Any] = {}
                    try:
                        parsed = json.loads(raw.decode("utf-8"))
                        if not isinstance(parsed, dict):
                            raise ControlProtocolError(
                                "bad_request", "message must be an object"
                            )
                        message = parsed
                        action = message.get("action")
                        if action in {"command", "heartbeat", "release", "set_mode"} and (
                            connection_session is None
                            or message.get("sessionId") != connection_session
                        ):
                            raise ControlProtocolError(
                                "invalid_session",
                                "control session is invalid for this connection",
                            )
                        result = self.state.handle(message, time.monotonic())
                        response = success_response(message, result)
                        if message.get("action") == "acquire":
                            connection_session = str(result["sessionId"])
                        elif message.get("action") == "release":
                            connection_session = None
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        response = error_response(
                            message,
                            ControlProtocolError("bad_request", "invalid JSON"),
                        )
                    except ControlProtocolError as error:
                        response = error_response(message, error)
                    try:
                        connection.sendall(
                            json.dumps(response, separators=(",", ":")).encode(
                                "utf-8"
                            )
                            + b"\n"
                        )
                    except OSError:
                        break
                    self._publish_snapshot()
        finally:
            self.state.disconnect(connection_session)
            self._publish_snapshot()
            with self._connections_lock:
                self._connections.discard(connection)
            connection.close()

    def stop(self) -> None:
        self._stop.set()
        if self._listener is not None:
            try:
                wake = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                wake.connect(str(self.socket_path))
                wake.close()
            except OSError:
                pass
        self._thread.join(timeout=5)
        with self._connections_lock:
            connections = list(self._connections)
        for connection in connections:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            connection.close()
        with self._connection_threads_lock:
            connection_threads = list(self._connection_threads)
        for connection_thread in connection_threads:
            connection_thread.join(timeout=2)
        self.socket_path.unlink(missing_ok=True)
        self.token_path.unlink(missing_ok=True)


def request(
    connection: socket.socket, message: Dict[str, Any]
) -> Dict[str, Any]:
    connection.sendall(
        json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n"
    )
    buffer = b""
    while b"\n" not in buffer:
        chunk = connection.recv(4096)
        if not chunk:
            raise RuntimeError("control server closed the connection")
        buffer += chunk
        if len(buffer) > MAX_MESSAGE_BYTES:
            raise RuntimeError("control response is too large")
    value = json.loads(buffer.split(b"\n", 1)[0].decode("utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError("control response is not an object")
    return value
