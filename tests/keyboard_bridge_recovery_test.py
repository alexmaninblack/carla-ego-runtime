import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import keyboard_control_bridge as bridge
from tools.external_control_client import ControlRequestRejected
from tools.external_control_client import ControlConnection
from tools.external_control_protocol import ExternalControlState, LocalControlServer


class RecoveryTests(unittest.TestCase):
    def test_first_command_wait_is_not_mistaken_for_manual_cancellation(self):
        self.assertEqual("manual", bridge.observed_mode("manual",
            dict(mode="safe_stop", reason="awaiting_command", held=False)))
        for reason in ("command_timeout", "operator_stop", "ownership_timeout"):
            self.assertEqual("safe_stop", bridge.observed_mode("manual",
                dict(mode="safe_stop", reason=reason, held=False)))
        self.assertEqual("safe_stop", bridge.observed_mode("manual",
            dict(mode="safe_stop", reason="awaiting_command", held=True)))

    def test_control_socket_responds_while_simulation_tick_is_stalled(self):
        # No world.tick/current_control call for the entire pause. The socket
        # worker must continue accepting input, heartbeats and an operator stop.
        import time
        with tempfile.TemporaryDirectory(prefix="control-") as directory:
            root = Path(directory)
            token = LocalControlServer.create_token_file(root / "token")
            state = ExternalControlState(token, .25, 1)
            server = LocalControlServer(root / "socket", root / "token", state)
            server.start()
            connection = None
            try:
                connection = ControlConnection(root / "socket", root / "token", "stall-test", protocol_version=3)
                connection.set_mode("manual")
                for _ in range(12):
                    connection.command(.2, 0, 0)
                    self.assertEqual("manual", connection.heartbeat()["mode"])
                    time.sleep(.1)
                connection.set_mode("safe_stop")
                applied = state.current_control(time.monotonic())
                self.assertEqual((applied.throttle, applied.brake), (0, 1))
                self.assertEqual("operator_stop", applied.reason)
                self.assertEqual(0, state.snapshot()["disconnects"])
            finally:
                if connection is not None:
                    connection.close()
                server.stop()

    def test_real_socket_remains_usable_after_invalid_pedal_message(self):
        with tempfile.TemporaryDirectory(prefix="control-") as directory:
            root = Path(directory)
            token = LocalControlServer.create_token_file(root / "token")
            state = ExternalControlState(token, .25, 1)
            server = LocalControlServer(root / "socket", root / "token", state)
            server.start()
            connection = None
            try:
                connection = ControlConnection(root / "socket", root / "token", "test", protocol_version=3)
                connection.set_mode("manual")
                self.assertFalse(bridge.manual_command(connection, dict(throttle=.1, brake=.1, steering=0)))
                self.assertEqual("safe_stop", connection.heartbeat()["mode"])
                connection.set_mode("manual")
                self.assertTrue(bridge.manual_command(connection, dict(throttle=.1, brake=0, steering=0)))
                self.assertEqual("manual", connection.heartbeat()["mode"])
                self.assertEqual(0, state.snapshot()["disconnects"])
            finally:
                if connection is not None:
                    connection.close()
                server.stop()

    def test_rejected_pedal_command_stops_without_closing_connection(self):
        connection = Mock()
        connection.command.side_effect = ControlRequestRejected({"error": {"code": "invalid_command"}})
        with patch.object(bridge, "emit") as emit:
            self.assertFalse(bridge.manual_command(connection, dict(throttle=.1, brake=.2, steering=0)))
            connection.set_mode.assert_called_once_with("safe_stop")
            connection.close.assert_not_called()
            emit.assert_called_once_with("mode_changed", mode="safe_stop", reason="command_rejected")

    def test_transport_and_lost_session_do_not_masquerade_as_input_rejection(self):
        for error in (OSError("closed"), ControlRequestRejected({"error": {"code": "invalid_session"}})):
            connection = Mock()
            connection.command.side_effect = error
            with self.assertRaises(type(error)):
                bridge.manual_command(connection, dict(throttle=0, brake=1, steering=0))
            connection.set_mode.assert_not_called()

    def test_failed_safe_stop_is_not_reported_as_recovered(self):
        connection = Mock()
        connection.command.side_effect = ControlRequestRejected({"error": {"code": "invalid_command"}})
        connection.set_mode.side_effect = OSError("closed")
        with patch.object(bridge, "emit") as emit, self.assertRaises(OSError):
            bridge.manual_command(connection, dict(throttle=.1, brake=.2, steering=0))
        emit.assert_not_called()


if __name__ == "__main__":
    unittest.main()
