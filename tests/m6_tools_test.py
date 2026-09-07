import importlib.util
import os
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]


def load_module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


RUNNER = load_module("m6_facts_runner_tested", REPOSITORY / "tools" / "run_m6.py")
CONTROLLER = load_module(
    "m6_facts_controller_tested",
    REPOSITORY / "tools" / "external_control_controller.py",
)


class M6ControllerFactsToolsTests(unittest.TestCase):
    def test_interactive_session_survives_hour_boundary_and_explicit_stop_remains(self):
        for elapsed in (0, 3599, 3600, 3600.037, 86401, 604800):
            self.assertFalse(CONTROLLER.session_expired(elapsed, 3600, True))
        with mock.patch.object(CONTROLLER, "STOP_REQUESTED", False):
            CONTROLLER.request_stop(None, None)
            self.assertTrue(CONTROLLER.STOP_REQUESTED)

    def test_noninteractive_runs_keep_configured_duration_cap(self):
        self.assertFalse(CONTROLLER.session_expired(3600, 3600))
        self.assertTrue(CONTROLLER.session_expired(3600.037, 3600))
        self.assertFalse(CONTROLLER.session_expired(20, 20))
        self.assertTrue(CONTROLLER.session_expired(20.05, 20))

    def test_only_interactive_runner_opts_out_of_duration_cap(self):
        interactive = (REPOSITORY / "tools" / "run_m6_interactive.py").read_text()
        bounded = (REPOSITORY / "tools" / "run_m6.py").read_text()
        self.assertIn('"--until-stopped",', interactive)
        self.assertIn('"session_lifetime": "until_stopped"', interactive)
        self.assertNotIn('"--until-stopped"', bounded)

    def test_duration_opt_out_requires_explicit_controller_flag(self):
        argv = ["controller", "--config", "config.json", "--python-api-root", "api",
                "--status-file", "status.json", "--socket-file", "control.sock",
                "--token-file", "control.token", "--facts-socket-file", "facts.sock",
                "--run-id", "test-run"]
        with mock.patch.object(CONTROLLER.sys, "argv", argv):
            self.assertFalse(CONTROLLER.parse_arguments().until_stopped)
        with mock.patch.object(CONTROLLER.sys, "argv", argv + ["--until-stopped"]):
            self.assertTrue(CONTROLLER.parse_arguments().until_stopped)

    def test_run_uses_one_short_owner_only_directory_for_both_sockets(self):
        control_directory, control_socket, token_file = RUNNER.create_control_paths()
        try:
            facts_socket = RUNNER.controller_facts_socket_path(control_directory)
            self.assertEqual(control_socket.parent, control_directory)
            self.assertEqual(token_file.parent, control_directory)
            self.assertEqual(facts_socket.parent, control_directory)
            self.assertEqual(control_directory.stat().st_mode & 0o777, 0o700)
            self.assertLessEqual(
                len(os.fsencode(facts_socket)),
                RUNNER.PORTABLE_UNIX_SOCKET_PATH_MAX,
            )
        finally:
            RUNNER.shutil.rmtree(control_directory, ignore_errors=True)

    def test_facts_socket_rejects_an_overlong_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / ("x" * 100)
            with self.assertRaisesRegex(RuntimeError, "Unix-domain limit"):
                RUNNER.controller_facts_socket_path(root)

    def test_controller_requires_explicit_run_and_facts_identity(self):
        parser_source = (
            REPOSITORY / "tools" / "external_control_controller.py"
        ).read_text(encoding="utf-8")
        self.assertIn('"--facts-socket-file", required=True', parser_source)
        self.assertIn('"--run-id", required=True', parser_source)

    def test_controller_emits_only_after_a_completed_real_tick(self):
        source = (
            REPOSITORY / "tools" / "external_control_controller.py"
        ).read_text(encoding="utf-8")
        tick = source.index("completed_frame_id = int(")
        snapshot = source.index("completed_snapshot = world.get_snapshot()", tick)
        send = source.index("facts_sender.send(", snapshot)
        self.assertLess(tick, snapshot)
        self.assertLess(snapshot, send)
        self.assertNotIn("controller-status.json", source[send:send + 500])


if __name__ == "__main__":
    unittest.main()
