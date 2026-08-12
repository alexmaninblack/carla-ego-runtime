import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY))

from tools.external_control_protocol import (
    ControlProtocolError,
    ExternalControlState,
    LocalControlServer,
    request,
)


def acquire(state, now=1.0):
    return state.handle(
        {
            "version": 1,
            "action": "acquire",
            "requestId": "a1",
            "clientId": "test-driver",
            "token": "secret",
        },
        now,
    )["sessionId"]


def command(state, session, sequence, now, **values):
    payload = {
        "version": 1,
        "action": "command",
        "requestId": f"c{sequence}",
        "sessionId": session,
        "sequence": sequence,
        "throttle": 0.0,
        "brake": 0.0,
        "steering": 0.0,
    }
    payload.update(values)
    return state.handle(payload, now)


class ExternalControlStateTests(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.state = ExternalControlState(
            "secret",
            command_timeout_seconds=0.25,
            ownership_timeout_seconds=1.0,
            event_sink=lambda event, fields: self.events.append((event, fields)),
        )

    def test_safe_stop_before_acquisition(self):
        control = self.state.current_control(0.0)
        self.assertTrue(control.safe_stop)
        self.assertEqual(control.brake, 1.0)
        self.assertEqual(control.reason, "startup")

    def test_authentication_and_exclusive_ownership(self):
        with self.assertRaisesRegex(ControlProtocolError, "unavailable") as failure:
            self.state.handle(
                {
                    "version": 1,
                    "action": "acquire",
                    "requestId": "bad",
                    "clientId": "test",
                    "token": "wrong",
                },
                0.0,
            )
        self.assertEqual(failure.exception.code, "unauthorized")
        acquire(self.state)
        with self.assertRaises(ControlProtocolError) as busy:
            acquire(self.state, 2.0)
        self.assertEqual(busy.exception.code, "control_busy")

    def test_command_is_applied_and_sequence_strictly_increases(self):
        session = acquire(self.state)
        command(self.state, session, 1, 1.1, throttle=0.5, steering=-0.25)
        control = self.state.current_control(1.2)
        self.assertFalse(control.safe_stop)
        self.assertEqual(control.throttle, 0.5)
        self.assertEqual(control.steering, -0.25)
        with self.assertRaises(ControlProtocolError) as replay:
            command(self.state, session, 1, 1.2)
        self.assertEqual(replay.exception.code, "invalid_sequence")

    def test_out_of_range_and_simultaneous_pedals_are_rejected(self):
        session = acquire(self.state)
        with self.assertRaises(ControlProtocolError) as range_error:
            command(self.state, session, 1, 1.1, steering=1.5)
        self.assertEqual(range_error.exception.code, "invalid_command")
        with self.assertRaises(ControlProtocolError):
            command(self.state, session, 2, 1.2, throttle=0.2, brake=0.1)

    def test_heartbeat_does_not_keep_an_old_command_active(self):
        session = acquire(self.state)
        command(self.state, session, 1, 1.1, throttle=0.4)
        self.state.handle(
            {
                "version": 1,
                "action": "heartbeat",
                "requestId": "h1",
                "sessionId": session,
            },
            1.3,
        )
        control = self.state.current_control(1.36)
        self.assertTrue(control.safe_stop)
        self.assertEqual(control.reason, "command_timeout")
        self.assertTrue(self.state.snapshot()["session_active"])

    def test_ownership_timeout_drops_session(self):
        session = acquire(self.state)
        command(self.state, session, 1, 1.1, throttle=0.4)
        control = self.state.current_control(2.2)
        self.assertTrue(control.safe_stop)
        self.assertEqual(control.reason, "ownership_timeout")
        self.assertFalse(self.state.snapshot()["session_active"])

    def test_release_and_disconnect_select_safe_stop(self):
        session = acquire(self.state)
        command(self.state, session, 1, 1.1, throttle=0.3)
        self.state.handle(
            {
                "version": 1,
                "action": "release",
                "requestId": "r1",
                "sessionId": session,
            },
            1.2,
        )
        self.assertEqual(self.state.current_control(1.2).reason, "release")
        new_session = acquire(self.state, 2.0)
        command(self.state, new_session, 1, 2.1, throttle=0.2)
        self.state.disconnect(new_session)
        self.assertEqual(self.state.current_control(2.2).reason, "disconnect")


class LocalControlServerTests(unittest.TestCase):
    def test_socket_and_token_are_private_and_cleanup_is_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            socket_path = root / "control.sock"
            token_path = root / "control.token"
            token = LocalControlServer.create_token_file(token_path)
            state = ExternalControlState(token, 0.25, 1.0)
            server = LocalControlServer(socket_path, token_path, state)
            server.start()
            self.assertEqual(token_path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(socket_path.stat().st_mode & 0o777, 0o600)
            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.connect(str(socket_path))
            response = request(
                connection,
                {
                    "version": 1,
                    "action": "acquire",
                    "requestId": "a1",
                    "clientId": "integration-test",
                    "token": token,
                },
            )
            self.assertEqual(response["status"], "ok")
            self.assertTrue(response["ts"].endswith("Z"))
            connection.close()
            deadline = time.monotonic() + 1
            while state.snapshot()["session_active"] and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(state.snapshot()["session_active"])
            server.stop()
            self.assertFalse(socket_path.exists())
            self.assertFalse(token_path.exists())

    def test_second_connection_observes_busy_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            socket_path = root / "control.sock"
            token_path = root / "control.token"
            token = LocalControlServer.create_token_file(token_path)
            state = ExternalControlState(token, 0.25, 1.0)
            server = LocalControlServer(socket_path, token_path, state)
            server.start()
            first = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            second = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            first.connect(str(socket_path))
            second.connect(str(socket_path))
            acquired = request(
                first,
                {
                    "version": 1,
                    "action": "acquire",
                    "requestId": "first",
                    "clientId": "first",
                    "token": token,
                },
            )
            self.assertEqual(acquired["status"], "ok")
            busy = request(
                second,
                {
                    "version": 1,
                    "action": "acquire",
                    "requestId": "second",
                    "clientId": "second",
                    "token": token,
                },
            )
            self.assertEqual(busy["error"]["code"], "control_busy")
            first.close()
            second.close()
            server.stop()


if __name__ == "__main__":
    unittest.main()
