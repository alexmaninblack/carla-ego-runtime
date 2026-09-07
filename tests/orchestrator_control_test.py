import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.external_control_protocol import ExternalControlState, ControlProtocolError

OP = "9112d447-ec87-45f0-92dc-d8537a129c46"
OTHER = "031ae4e1-954f-45f4-bb67-0416f16bd431"


class OrchestratorControlTests(unittest.TestCase):
    def manual_ready(self):
        session = self.state.handle(dict(version=2, action="acquire", requestId="ui",
            token="fixture-secret", clientId="native-ui"), 1)["sessionId"]
        self.call("safe_stop")
        self.observe()
        self.call("reset", at=1.11)
        self.state.current_control(1.12)
        self.observe(at=1.15, reset=1)
        self.call("manual_ready", at=1.16)
        return session

    def observe_manual(self, at=1.2):
        self.frame += 1
        self.state.observe_completed_frame(now=at, run_id="fixture-run", ego_actor_id=1,
            frame_id=self.frame, simulation_time=self.frame * .05, active_mode="manual",
            control_generation=self.state.current_control(at).mode_generation,
            reset_generation=1, speed_kmh=0, brake=1)

    def test_manual_ready_requires_real_frame_before_source_release(self):
        session = self.manual_ready()
        with self.assertRaises(ControlProtocolError):
            self.call("release_manual", at=1.17)
        heartbeat = self.state.handle(dict(version=2, action="heartbeat", requestId="heartbeat", sessionId=session), 1.18)
        self.assertTrue(heartbeat["held"])
        self.observe_manual()
        self.assertEqual("RELEASED", self.call("release_manual", at=1.21)["phase"])
        control = self.state.current_control(1.6)
        self.assertEqual("manual", control.mode)
        self.assertEqual((0, 1, 0), (control.throttle, control.brake, control.steering))
        self.assertFalse(control.safe_stop)

    def test_first_command_restores_timeout_and_disconnect_still_stops(self):
        session = self.manual_ready()
        self.observe_manual()
        self.call("release_manual", at=1.21)
        self.state.handle(dict(version=2, action="command", requestId="drive", sessionId=session,
            sequence=1, throttle=.1, brake=0, steering=0), 1.3)
        self.assertTrue(self.state.current_control(1.6).safe_stop)
        self.state.disconnect(session)
        self.assertTrue(self.state.current_control(1.61).safe_stop)

    def test_manual_ready_does_not_disable_ownership_timeout(self):
        self.manual_ready()
        self.observe_manual()
        self.call("release_manual", at=1.21)
        self.assertTrue(self.state.current_control(2.1).safe_stop)

    def test_manual_ready_requires_reset_and_native_session(self):
        self.call("safe_stop")
        self.observe()
        with self.assertRaises(ControlProtocolError):
            self.call("manual_ready", at=1.11)

    def setUp(self):
        self.state = ExternalControlState("fixture-secret", 0.25, 1.0)
        self.frame = 0

    def call(self, operation, at=1.0, identity=OP, **extra):
        payload = dict(version=3, action="orchestrate", requestId="fixture",
                       token="fixture-secret", operation=operation, operationId=identity)
        payload.update(extra)
        return self.state.handle(payload, at)

    def observe(self, at=1.1, reset=0, speed=0, brake=1):
        self.frame += 1
        self.state.observe_completed_frame(now=at, run_id="fixture-run", ego_actor_id=1,
            frame_id=self.frame, simulation_time=self.frame * 0.05, active_mode="safe_stop",
            control_generation=self.state.current_control(at).mode_generation,
            reset_generation=reset, speed_kmh=speed, brake=brake)

    def test_stop_needs_completed_stopped_frame(self):
        self.assertEqual("STOPPING", self.call("safe_stop")["phase"])
        with self.assertRaises(ControlProtocolError):
            self.call("reset")
        self.observe(speed=10)
        self.assertEqual("STOPPING", self.call("status", at=1.1)["phase"])
        self.observe(at=1.15)
        self.assertEqual("SAFE_STOP", self.call("status", at=1.15)["phase"])

    def test_repeat_and_conflicting_owner(self):
        self.call("safe_stop")
        generation = self.state.current_control(1).mode_generation
        self.call("safe_stop")
        self.assertEqual(generation, self.state.current_control(1).mode_generation)
        with self.assertRaises(ControlProtocolError):
            self.call("safe_stop", identity=OTHER)

    def test_auth_and_exact_fields(self):
        for fields in ({"token": "wrong"}, {"force": True}, {"version": 2}):
            with self.assertRaises(ControlProtocolError):
                self.call("safe_stop", **fields)
        self.assertFalse(self.call("status")["held"])

    def test_ui_session_preserved_but_driving_interlocked(self):
        session = self.state.handle(dict(version=2, action="acquire", requestId="ui",
            token="fixture-secret", clientId="native-ui"), 1)["sessionId"]
        self.call("safe_stop")
        self.assertEqual("native-ui", self.state.snapshot()["owner"])
        with self.assertRaises(ControlProtocolError):
            self.state.handle(dict(version=2, action="set_mode", requestId="ui-mode",
                sessionId=session, mode="autopilot"), 1.1)
        heartbeat = self.state.handle(dict(version=2, action="heartbeat", requestId="ui-heartbeat",
            sessionId=session), 1.1)
        self.assertEqual("safe_stop", heartbeat["mode"])

    def test_reset_once_and_physical_generation(self):
        self.call("safe_stop")
        self.observe()
        self.assertEqual("RESETTING", self.call("reset", at=1.11)["phase"])
        self.call("reset", at=1.11)
        self.assertTrue(self.state.current_control(1.12).reset_requested)
        self.assertFalse(self.state.current_control(1.12).reset_requested)
        self.observe(at=1.15, reset=0)
        self.assertEqual("RESETTING", self.call("status", at=1.15)["phase"])
        self.observe(at=1.2, reset=1)
        self.assertEqual("RESET", self.call("status", at=1.2)["phase"])
        self.call("reset", at=1.21)
        self.assertFalse(self.state.current_control(1.21).reset_requested)

    def test_stale_frame_blocks_reset_and_release(self):
        self.call("safe_stop")
        self.observe()
        for operation in ("reset", "release"):
            with self.assertRaises(ControlProtocolError):
                self.call(operation, at=2)
        self.assertTrue(self.call("status", at=2)["held"])

    def test_release_is_stopped_and_idempotent(self):
        self.call("safe_stop")
        self.observe()
        self.call("release", at=1.11)
        self.assertEqual("RELEASED", self.call("release", at=1.12)["phase"])
        self.assertEqual("RELEASED", self.call("safe_stop", at=1.12)["phase"])
        self.assertTrue(self.state.current_control(1.12).safe_stop)

    def test_other_operation_cannot_reset_or_release(self):
        self.call("safe_stop")
        self.observe()
        for operation in ("reset", "release"):
            with self.assertRaises(ControlProtocolError):
                self.call(operation, at=1.11, identity=OTHER)

    def test_hybrid_scene_not_silently_reset(self):
        self.state = ExternalControlState("fixture-secret", 0.25, 1.0,
                                          orchestration_reset_supported=False)
        self.call("safe_stop")
        self.observe()
        with self.assertRaisesRegex(ControlProtocolError, "unavailable"):
            self.call("reset", at=1.11)


if __name__ == "__main__":
    unittest.main()
