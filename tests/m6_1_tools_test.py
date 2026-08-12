import importlib.util
import os
import re
import tempfile
import time
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]


def load_module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


CONTROLLER = load_module(
    "m61_config_tested", REPOSITORY / "tools" / "behavior_agent_controller.py"
)
RUNNER = load_module(
    "m61_interactive_tested", REPOSITORY / "tools" / "run_m6_interactive.py"
)
LAUNCHER = load_module(
    "m61_launcher_tested", REPOSITORY / "tools" / "launch_m6_1.py"
)


class M61ToolTests(unittest.TestCase):
    def test_keyboard_directions_are_arranged_as_a_cross(self):
        source = (REPOSITORY / "tools" / "KeyboardControl.swift").read_text()

        def origin(name):
            match = re.search(
                rf"{name} = NSRect\(x: ([0-9]+), y: ([0-9]+)", source
            )
            self.assertIsNotNone(match, f"missing {name}")
            return tuple(int(value) for value in match.groups())

        throttle = origin("throttleKeyRect")
        brake = origin("brakeKeyRect")
        steer_left = origin("steerLeftKeyRect")
        steer_right = origin("steerRightKeyRect")
        self.assertEqual(throttle[0], brake[0])
        self.assertGreater(throttle[1], steer_left[1])
        self.assertEqual(steer_left[1], steer_right[1])
        self.assertLess(brake[1], steer_left[1])
        self.assertLess(steer_left[0], throttle[0])
        self.assertGreater(steer_right[0], throttle[0])

    def test_status_and_actions_use_centered_card_text(self):
        source = (REPOSITORY / "tools" / "KeyboardControl.swift").read_text()
        self.assertIn("centeredText(active ? reason", source)
        self.assertIn("centeredText(title, in: rect", source)

    def test_keyboard_configuration_is_external_and_operator_bounded(self):
        config = CONTROLLER.load_config(
            REPOSITORY / "config" / "m6_1_town10hd_keyboard.json"
        )
        external = config["controller"]["external_control"]
        self.assertEqual(config["controller"]["type"], "external_control")
        self.assertEqual(external["command_timeout_seconds"], 0.25)
        self.assertEqual(external["ownership_timeout_seconds"], 1.0)
        self.assertEqual(external["maximum_session_seconds"], 3600)

    def test_keyboard_command_uses_private_run_artifact_paths(self):
        arguments = type(
            "Arguments",
            (),
            {"python": Path("python"), "keyboard_ui": Path("KeyboardControl")},
        )()
        command = RUNNER.keyboard_command(
            arguments, Path("run/control.sock"), Path("run/control.token")
        )
        self.assertEqual(command[0], "KeyboardControl")
        self.assertIn("keyboard_control_bridge.py", command[1])
        self.assertIn("run/control.sock", command[1])
        self.assertIn("run/control.token", command[1])

    def test_native_keyboard_app_paths_are_launcher_owned(self):
        source = (REPOSITORY / "tools" / "launch_m6_1.py").read_text()
        self.assertIn("xcrun", source)
        self.assertIn("KeyboardControl.swift", str(REPOSITORY / "tools" / "KeyboardControl.swift"))
        self.assertIn("codesign", source)

    def test_native_keyboard_app_reuses_a_current_local_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "KeyboardControl.swift"
            info_source = root / "Info-source.plist"
            app = root / "KeyboardControl.app"
            executable = app / "Contents" / "MacOS" / "KeyboardControl"
            info = app / "Contents" / "Info.plist"
            executable.parent.mkdir(parents=True)
            source.write_text("source")
            info_source.write_text("info")
            executable.write_text("binary")
            info.write_text("installed info")
            future = time.time() + 10
            os.utime(executable, (future, future))
            os.utime(info, (future, future))
            arguments = type(
                "Arguments",
                (),
                {
                    "keyboard_app": app,
                    "keyboard_source": source,
                    "keyboard_info": info_source,
                    "run_root": root / "runs",
                },
            )()
            self.assertEqual(LAUNCHER.build_keyboard_app(arguments), executable)

    def test_startup_timeline_preserves_marks_from_both_layers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "startup-timeline.json"
            started_at = time.time()
            LAUNCHER.timeline_mark(path, started_at, "launcher")
            RUNNER.timeline_mark(path, started_at, "orchestrator")
            payload = RUNNER.M5.read_json(path)
            self.assertEqual(
                [entry["stage"] for entry in payload["stages"]],
                ["launcher", "orchestrator"],
            )

    def test_manual_cleanup_requires_stopped_owner_and_removed_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            run_directory = Path(directory)
            RUNNER.M5.atomic_write_json(
                run_directory / "controller-status.json",
                {
                    "state": "stopped",
                    "control": {"session_active": False, "owner": None},
                },
            )
            self.assertTrue(LAUNCHER.manual_run_cleanup_is_valid(run_directory))
            (run_directory / "control.token").write_text("secret")
            self.assertFalse(LAUNCHER.manual_run_cleanup_is_valid(run_directory))


if __name__ == "__main__":
    unittest.main()
