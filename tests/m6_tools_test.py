import importlib.util
import os
import tempfile
import unittest
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
