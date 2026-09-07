#!/usr/bin/env python3
"""Thread-safe M6 external-control contract and local JSON-lines server."""

from __future__ import annotations

import ctypes
import datetime as dt
import hmac
import json
import math
import os
import secrets
import socket
import struct
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, Optional, Tuple


CONTRACT_VERSION = 3
SUPPORTED_VERSIONS = {1, 2, 3}
DRIVE_MODES = {"safe_stop", "manual", "autopilot", "scenario"}
DEFAULT_AVAILABLE_MODES = {"safe_stop", "manual", "autopilot"}
MAX_MESSAGE_BYTES = 16 * 1024
MAX_CONTROL_FACTS_BODY_BYTES = 4096
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
    mode_generation: int
    reset_requested: bool = False


def _uint64(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an unsigned integer")
    if value < 0 or value > (1 << 64) - 1:
        raise ValueError(f"{name} must fit uint64")
    return value


def encode_controller_gateway_record(
    *,
    run_id: str,
    ego_actor_id: int,
    frame_id: int,
    simulation_time: float,
    active_mode: str,
    transition_state: str,
    control_generation: int,
    reset_generation: int,
    reset_in_progress: bool,
    reset_discontinuity: bool,
) -> bytes:
    if not isinstance(run_id, str) or not run_id or "\x00" in run_id:
        raise ValueError("run_id must be a non-empty string")
    if active_mode not in {"safe_stop", "scenario", "manual", "autopilot"}:
        raise ValueError("active_mode is invalid")
    if transition_state not in {"stable", "preparing", "failed"}:
        raise ValueError("transition_state is invalid")
    if (
        isinstance(simulation_time, bool)
        or not isinstance(simulation_time, (int, float))
        or not math.isfinite(float(simulation_time))
        or float(simulation_time) < 0.0
    ):
        raise ValueError("simulation_time must be finite and non-negative")
    if not isinstance(reset_in_progress, bool) or not isinstance(
        reset_discontinuity, bool
    ):
        raise ValueError("reset flags must be boolean")
    if reset_in_progress and reset_discontinuity:
        raise ValueError("reset cannot be in progress and discontinuous")
    record = {
        "schemaVersion": 1,
        "runId": run_id,
        "egoActorId": _uint64(ego_actor_id, "ego_actor_id"),
        "frameId": _uint64(frame_id, "frame_id"),
        "simulationTime": float(simulation_time),
        "activeMode": active_mode.upper(),
        "transitionState": transition_state.upper(),
        "controlGeneration": _uint64(control_generation, "control_generation"),
        "resetGeneration": _uint64(reset_generation, "reset_generation"),
        "resetInProgress": reset_in_progress,
        "resetDiscontinuity": reset_discontinuity,
    }
    payload = json.dumps(
        record, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    if len(payload) > MAX_CONTROL_FACTS_BODY_BYTES:
        raise ValueError("controller facts body exceeds 4096 bytes")
    return payload


class ControllerFactsState:
    """Actual applied controller/reset state, independent of requested mode."""

    def __init__(self) -> None:
        self.active_mode = "safe_stop"
        self.transition_state = "stable"
        self.control_generation = 0
        self.reset_generation = 0
        self.reset_in_progress = False
        self._requested_mode = "safe_stop"
        self._discontinuity_pending = False

    def begin_transition(
        self, requested_mode: str, control_generation: int, reset_required: bool
    ) -> bool:
        if requested_mode not in DRIVE_MODES:
            raise ValueError("requested mode is invalid")
        generation = _uint64(control_generation, "control_generation")
        if (
            generation == self.control_generation
            and requested_mode == self.active_mode
            and requested_mode != "scenario"
        ):
            return False
        if generation <= self.control_generation:
            raise ValueError("non-idempotent control generation must increase")
        self.control_generation = generation
        self._requested_mode = requested_mode
        self.transition_state = "preparing"
        self.reset_in_progress = bool(reset_required)
        return True

    def complete_transition(self, applied_mode: str, reset_completed: bool) -> None:
        if self.transition_state != "preparing" or applied_mode != self._requested_mode:
            raise ValueError("no matching transition is being prepared")
        if self.reset_in_progress != bool(reset_completed):
            raise ValueError("reset completion does not match the transition")
        if reset_completed:
            if self.reset_generation == (1 << 64) - 1:
                raise OverflowError("reset generation cannot wrap")
            self.reset_generation += 1
            self._discontinuity_pending = True
        self.active_mode = applied_mode
        self.transition_state = "stable"
        self.reset_in_progress = False

    def fail_transition(self) -> None:
        if self.transition_state != "preparing":
            raise ValueError("no transition is being prepared")
        self.active_mode = "safe_stop"
        self.transition_state = "failed"
        self.reset_in_progress = False

    def completed_frame(
        self,
        *,
        run_id: str,
        ego_actor_id: int,
        frame_id: int,
        simulation_time: float,
    ) -> bytes:
        payload = encode_controller_gateway_record(
            run_id=run_id,
            ego_actor_id=ego_actor_id,
            frame_id=frame_id,
            simulation_time=simulation_time,
            active_mode=self.active_mode,
            transition_state=self.transition_state,
            control_generation=self.control_generation,
            reset_generation=self.reset_generation,
            reset_in_progress=self.reset_in_progress,
            reset_discontinuity=self._discontinuity_pending,
        )
        self._discontinuity_pending = False
        if self.transition_state == "failed":
            self.transition_state = "stable"
        return payload


def frame_controller_gateway_record(payload: bytes) -> bytes:
    if (
        not isinstance(payload, bytes)
        or not payload
        or len(payload) > MAX_CONTROL_FACTS_BODY_BYTES
    ):
        raise ValueError("controller facts body is invalid")
    return struct.pack("!I", len(payload)) + payload


def _peer_effective_uid(connection: socket.socket) -> int:
    if sys.platform == "darwin":
        peer_uid = ctypes.c_uint()
        peer_gid = ctypes.c_uint()
        getpeereid = ctypes.CDLL(None, use_errno=True).getpeereid
        getpeereid.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
        ]
        getpeereid.restype = ctypes.c_int
        if getpeereid(
            connection.fileno(), ctypes.byref(peer_uid), ctypes.byref(peer_gid)
        ) != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
        return int(peer_uid.value)
    if sys.platform.startswith("linux"):
        credentials = connection.getsockopt(
            socket.SOL_SOCKET,
            getattr(socket, "SO_PEERCRED", 17),
            struct.calcsize("3i"),
        )
        _, peer_uid, _ = struct.unpack("3i", credentials)
        return int(peer_uid)
    raise OSError("controller facts peer credentials are unsupported")


class ControllerFactsStreamSender:
    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        self._socket: Optional[socket.socket] = None
        self._connection_attempted = False
        self._available = True
        self.sent = 0
        self.dropped = 0

    def send(self, payload: bytes) -> bool:
        frame = frame_controller_gateway_record(payload)
        if not self._connect_once():
            self.dropped += 1
            return False
        try:
            sent = self._socket.send(frame, getattr(socket, "MSG_NOSIGNAL", 0))
        except (BlockingIOError, BrokenPipeError, ConnectionResetError, OSError):
            self.dropped += 1
            self._make_unavailable()
            return False
        if sent != len(frame):
            self.dropped += 1
            self._make_unavailable()
            return False
        self.sent += 1
        return True

    def _connect_once(self) -> bool:
        if self._socket is not None:
            return True
        if self._connection_attempted or not self._available:
            return False
        self._connection_attempted = True
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.setblocking(False)
        try:
            connection.connect(str(self.socket_path))
            if _peer_effective_uid(connection) != os.geteuid():
                raise PermissionError("controller facts peer UID does not match")
        except (BlockingIOError, FileNotFoundError, ConnectionRefusedError, OSError):
            connection.close()
            self._available = False
            return False
        self._socket = connection
        return True

    def _make_unavailable(self) -> None:
        self._available = False
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def close(self) -> None:
        self._make_unavailable()


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
        available_modes: Optional[set[str]] = None,
        orchestration_reset_supported: bool = True,
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
        self._available_modes = set(
            DEFAULT_AVAILABLE_MODES if available_modes is None else available_modes
        )
        self._available_modes.add("safe_stop")
        if not self._available_modes.issubset(DRIVE_MODES):
            raise ValueError("available modes contain an unsupported mode")
        self._lock = threading.Lock()
        self._session_id: Optional[str] = None
        self._client_id: Optional[str] = None
        self._last_sequence = 0
        self._last_command_at: Optional[float] = None
        self._last_heartbeat_at: Optional[float] = None
        self._command = dict(SAFE_CONTROL)
        self._safe_stop_reason = "startup"
        self._mode = "safe_stop"
        self._mode_generation = 0
        self._orchestration_reset_supported = orchestration_reset_supported
        self._orchestration: Optional[Dict[str, Any]] = None
        self._completed_frame: Optional[Dict[str, Any]] = None
        self._completed_at = 0.0
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
            "scenario_activations": 0,
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

    def _advance_mode_generation(self) -> None:
        if self._mode_generation == (1 << 64) - 1:
            raise RuntimeError("control mode generation cannot wrap")
        self._mode_generation += 1

    def _applied_mode(self) -> str:
        if self._mode == "manual" and self._safe_stop_reason not in {"command", "manual_ready"}:
            return "safe_stop"
        return self._mode

    def _drop_ownership(self, reason: str) -> None:
        self._mode = "safe_stop"
        self._advance_mode_generation()
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
            raise ControlProtocolError("bad_request", "version must be 1, 2, or 3")
        action = _string(message, "action")
        _string(message, "requestId")

        if action == "orchestrate":
            return self._orchestrate_locked(message, now)

        held = self._orchestration is not None and self._orchestration["held"]
        if held and (action in {"acquire", "command"} or
                     (action == "set_mode" and message.get("mode") != "safe_stop")):
            raise ControlProtocolError("orchestration_busy", "vehicle is held in safe stop")

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
            return {
                "status": "ok",
                "sessionId": self._session_id,
                "availableModes": sorted(self._available_modes),
            }

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
            recovering_from_safe_stop = self._safe_stop_reason != "command"
            self._last_sequence = sequence
            self._last_command_at = now
            self._last_heartbeat_at = now
            self._command = {
                "throttle": throttle,
                "brake": brake,
                "steering": steering,
            }
            self._safe_stop_reason = "command"
            if recovering_from_safe_stop:
                self._advance_mode_generation()
            self._metrics["commands"] += 1
            return {"status": "ok", "sequence": sequence}

        if action == "set_mode":
            if version < 2:
                raise ControlProtocolError(
                    "bad_request", "set_mode requires protocol version 2 or 3"
                )
            self._require_session(message)
            mode = _string(message, "mode")
            if mode not in DRIVE_MODES:
                raise ControlProtocolError(
                    "invalid_mode",
                    "mode must be safe_stop, manual, autopilot, or scenario",
                )
            if mode not in self._available_modes:
                raise ControlProtocolError(
                    "mode_unavailable", f"{mode} mode is not configured"
                )
            previous_mode = self._mode
            if mode == previous_mode and mode != "scenario":
                self._last_heartbeat_at = now
                return {
                    "status": "ok",
                    "mode": mode,
                    "modeGeneration": self._mode_generation,
                }
            if self._mode_validator is not None:
                unavailable = self._mode_validator(mode)
                if unavailable is not None:
                    raise ControlProtocolError("mode_unavailable", unavailable)
            self._mode = mode
            self._advance_mode_generation()
            self._last_heartbeat_at = now
            if mode == "manual":
                self._select_safe_stop("awaiting_command")
                self._metrics["manual_activations"] += 1
            elif mode == "autopilot":
                self._command = dict(SAFE_CONTROL)
                self._last_command_at = None
                self._safe_stop_reason = "autopilot"
                self._metrics["autopilot_activations"] += 1
            elif mode == "scenario":
                self._command = dict(SAFE_CONTROL)
                self._last_command_at = None
                self._safe_stop_reason = "scenario"
                self._metrics["scenario_activations"] += 1
            else:
                self._select_safe_stop("operator_stop")
            self._metrics["mode_changes"] += 1
            self._event(
                "drive_mode_changed",
                previous_mode=previous_mode,
                mode=mode,
                client_id=self._client_id,
            )
            return {
                "status": "ok",
                "mode": mode,
                "modeGeneration": self._mode_generation,
            }

        if action == "heartbeat":
            self._require_session(message)
            self._last_heartbeat_at = now
            self._metrics["heartbeats"] += 1
            return {
                "status": "ok",
                "mode": self._applied_mode(),
                "modeGeneration": self._mode_generation,
                "reason": self._safe_stop_reason,
                "held": held,
            }

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
                elif self._mode == "manual" and self._safe_stop_reason != "manual_ready" and (
                    self._last_command_at is None
                    or now - self._last_command_at > self._command_timeout
                ):
                    if self._safe_stop_reason != "command_timeout":
                        self._metrics["command_timeouts"] += 1
                        self._advance_mode_generation()
                    self._select_safe_stop("command_timeout")
            return AppliedControl(
                throttle=float(self._command["throttle"]),
                brake=float(self._command["brake"]),
                steering=float(self._command["steering"]),
                sequence=self._last_sequence,
                safe_stop=(
                    self._mode == "safe_stop"
                    or (self._mode == "manual" and self._safe_stop_reason not in {"command", "manual_ready"})
                ),
                reason=self._safe_stop_reason,
                mode=self._applied_mode(),
                mode_generation=self._mode_generation,
                reset_requested=self._consume_reset_locked(),
            )

    def _consume_reset_locked(self) -> bool:
        operation = self._orchestration
        if operation and operation["held"] and operation.get("resetPending"):
            operation["resetPending"] = False
            return True
        return False

    def _orchestration_status_locked(self, now: float) -> Dict[str, Any]:
        operation = self._orchestration
        return {"status": "ok", "operationId": operation["id"] if operation else None,
                "held": bool(operation and operation["held"]),
                "phase": operation["phase"] if operation else "IDLE",
                "fresh": self._completed_frame is not None and 0 <= now - self._completed_at <= 0.25,
                "frame": dict(self._completed_frame) if self._completed_frame else None}

    def _orchestrate_locked(self, message: Dict[str, Any], now: float) -> Dict[str, Any]:
        if message.get("version") != 3 or set(message) != {
                "version", "action", "requestId", "token", "operation", "operationId"}:
            raise ControlProtocolError("bad_request", "invalid orchestration request")
        if not hmac.compare_digest(_string(message, "token"), self._token):
            raise ControlProtocolError("unauthorized", "control is unavailable")
        operation_id = _string(message, "operationId")
        try:
            if str(uuid.UUID(operation_id)) != operation_id:
                raise ValueError()
        except ValueError:
            raise ControlProtocolError("bad_request", "invalid operation identity") from None
        action = _string(message, "operation")
        if action == "status":
            return self._orchestration_status_locked(now)
        current = self._orchestration
        if action == "safe_stop":
            if current and current["id"] == operation_id:
                return self._orchestration_status_locked(now)
            if current and current["held"]:
                raise ControlProtocolError("orchestration_busy", "another operation holds safe stop")
            self._mode = "safe_stop"
            self._advance_mode_generation()
            self._select_safe_stop("orchestrator")
            self._orchestration = {"id": operation_id, "held": True, "phase": "STOPPING"}
            return self._orchestration_status_locked(now)
        if action not in {"reset", "release", "manual_ready", "release_manual"}:
            raise ControlProtocolError("bad_request", "unsupported orchestration operation")
        if not current or current["id"] != operation_id:
            raise ControlProtocolError("orchestration_mismatch", "operation does not own safe stop")
        if action in {"release", "release_manual"} and not current["held"]:
            return self._orchestration_status_locked(now)
        if not current["held"]:
            raise ControlProtocolError("orchestration_mismatch", "operation already released")
        if action == "reset" and current["phase"] in {"RESETTING", "RESET"}:
            return self._orchestration_status_locked(now)
        if action == "manual_ready" and current["phase"] in {"MANUAL_PREPARING", "MANUAL_READY"}:
            return self._orchestration_status_locked(now)
        result = self._orchestration_status_locked(now)
        frame = result["frame"]
        if action == "release_manual":
            if (current["phase"] != "MANUAL_READY" or not result["fresh"] or not self._session_id
                    or not frame or frame["activeMode"] != "MANUAL" or frame["speedKmh"] > .5
                    or frame["brake"] < .99 or frame["controlGeneration"] != self._mode_generation):
                raise ControlProtocolError("manual_ready_not_confirmed", "completed stationary manual frame and native operator session required")
            current.update(held=False, phase="RELEASED")
            return self._orchestration_status_locked(now)
        if (current["phase"] not in {"SAFE_STOP", "RESET"} or not result["fresh"] or
                not frame or frame["activeMode"] != "SAFE_STOP" or frame["speedKmh"] > 0.5 or
                frame["brake"] < 0.99 or frame["controlGeneration"] != self._mode_generation):
            raise ControlProtocolError("safe_stop_not_confirmed", "completed stopped frame is required")
        if action == "manual_ready":
            if current["phase"] != "RESET" or not self._session_id:
                raise ControlProtocolError("manual_ready_not_confirmed", "post-reset frame and native operator session required")
            self._mode = "manual"
            self._advance_mode_generation()
            self._command = dict(SAFE_CONTROL)
            self._last_command_at = None
            self._safe_stop_reason = "manual_ready"
            current.update(phase="MANUAL_PREPARING")
        elif action == "reset":
            if not self._orchestration_reset_supported:
                raise ControlProtocolError("reset_unavailable", "standalone reset is unavailable for this scene")
            if frame["resetGeneration"] == (1 << 64) - 1:
                raise ControlProtocolError("reset_unavailable", "reset generation is exhausted")
            self._advance_mode_generation()
            current.update(phase="RESETTING", resetPending=True, resetBaseline=frame["resetGeneration"])
        else:
            current.update(held=False, phase="RELEASED")
        return self._orchestration_status_locked(now)

    def observe_completed_frame(self, *, now: float, run_id: str, ego_actor_id: int,
                                frame_id: int, simulation_time: float, active_mode: str,
                                control_generation: int, reset_generation: int,
                                speed_kmh: float, brake: float) -> None:
        """Called only by the tick owner after a real completed CARLA frame."""
        if not all(math.isfinite(value) for value in (now, simulation_time, speed_kmh, brake)):
            raise ValueError("non-finite completed frame")
        if speed_kmh < 0 or not 0 <= brake <= 1:
            raise ValueError("invalid completed motion")
        with self._lock:
            if self._completed_frame and frame_id <= self._completed_frame["frameId"]:
                raise ValueError("completed frames must advance")
            self._completed_frame = {
                "runId": run_id, "egoActorId": ego_actor_id, "frameId": frame_id,
                "simulationTime": simulation_time, "activeMode": active_mode.upper(),
                "controlGeneration": control_generation, "resetGeneration": reset_generation,
                "speedKmh": speed_kmh, "brake": brake}
            self._completed_at = now
            current = self._orchestration
            if (current and current["held"] and current["phase"] == "MANUAL_PREPARING"
                    and active_mode == "manual" and self._safe_stop_reason == "manual_ready"
                    and control_generation == self._mode_generation and speed_kmh <= .5 and brake >= .99):
                current["phase"] = "MANUAL_READY"
            stopped = (active_mode == "safe_stop" and control_generation == self._mode_generation
                       and speed_kmh <= 0.5 and brake >= 0.99)
            if current and current["held"] and stopped:
                if current["phase"] == "STOPPING":
                    current["phase"] = "SAFE_STOP"
                elif current["phase"] == "RESETTING" and not current.get("resetPending"):
                    if reset_generation == current["resetBaseline"] + 1:
                        current["phase"] = "RESET"

    def force_safe_stop(self, reason: str) -> None:
        if not reason:
            raise ValueError("safe-stop reason must not be empty")
        with self._lock:
            previous_mode = self._mode
            self._mode = "safe_stop"
            self._advance_mode_generation()
            self._select_safe_stop(reason)
            self._metrics["mode_changes"] += 1
            self._event(
                "drive_mode_changed",
                previous_mode=previous_mode,
                mode="safe_stop",
                reason=reason,
                client_id=self._client_id,
            )

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return {
                "owner": self._client_id,
                "session_active": self._session_id is not None,
                "last_sequence": self._last_sequence,
                "safe_stop_reason": self._safe_stop_reason,
                "mode": self._applied_mode(),
                "mode_generation": self._mode_generation,
                "available_modes": sorted(self._available_modes),
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
