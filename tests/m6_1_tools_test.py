import importlib.util
import os
import re
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


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
M6_RUNNER = load_module("m6_acceptance_tested", REPOSITORY / "tools" / "run_m6.py")


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
        self.assertIn("centeredText(statusDetail", source)
        self.assertIn("centeredText(title, in: rect", source)

    def test_live_handover_ui_has_explicit_modes_and_hybrid_scenario(self):
        source = (REPOSITORY / "tools" / "KeyboardControl.swift").read_text()
        self.assertIn('onMode?("manual")', source)
        self.assertIn('onMode?("autopilot")', source)
        self.assertIn('onMode?("safe_stop")', source)
        self.assertIn('onMode?("scenario")', source)
        self.assertIn('view.mode == "manual"', source)
        self.assertIn("START SCRIPTED SCENARIO", source)
        self.assertIn("scripted and autopilot modes continue", source)

    def test_keyboard_configuration_is_external_and_operator_bounded(self):
        config = CONTROLLER.load_config(
            REPOSITORY / "config" / "m6_1_town10hd_keyboard.json"
        )
        external = config["controller"]["external_control"]
        self.assertEqual(config["controller"]["type"], "external_control")
        self.assertEqual(external["command_timeout_seconds"], 0.25)
        self.assertEqual(external["ownership_timeout_seconds"], 1.0)
        self.assertEqual(external["maximum_session_seconds"], 3600)
        self.assertNotIn("autopilot", config["controller"])

    def test_live_handover_configuration_is_deterministic(self):
        config = CONTROLLER.load_config(
            REPOSITORY / "config" / "m6_2_town10hd_handover.json"
        )
        autopilot = config["controller"]["autopilot"]
        self.assertEqual(autopilot["traffic_manager_port"], 8000)
        self.assertEqual(autopilot["random_seed"], 42)
        self.assertFalse(autopilot["automatic_lane_change"])
        self.assertAlmostEqual(config["simulation"]["fixed_delta_seconds"], 1 / 30)
        self.assertEqual(config["runtime"]["chase_camera_update_hz"], 30)

    def test_hybrid_brake_event_reuses_external_control(self):
        config = CONTROLLER.load_config(
            REPOSITORY / "config" / "brake_event_hybrid_town10hd.json"
        )
        self.assertEqual(config["controller"]["type"], "external_control")
        self.assertEqual(
            config["controller"]["scenario"]["id"],
            "stationary-obstacle-braking-v1",
        )
        self.assertEqual(
            config["controller"]["external_control"]["manual_handover_seconds"],
            0.3,
        )
        self.assertIn("autopilot", config["controller"])

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

    def test_bridge_discards_queued_manual_commands_after_a_mode_change(self):
        source = (
            REPOSITORY / "tools" / "keyboard_control_bridge.py"
        ).read_text(encoding="utf-8")
        self.assertIn('current_mode = "safe_stop"', source)
        self.assertIn('elif current_mode == "manual":', source)
        self.assertIn("protocol_version=3", source)
        self.assertIn('server_mode != current_mode', source)

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

    def test_reused_simulator_identity_requires_exact_project_and_rpc_port(self):
        editor = Path("/opt/UnrealEditor")
        project = Path("/work/CarlaUnreal.uproject")
        command = (
            f"{editor} {project} /Game/Carla/Maps/Town10HD_Opt "
            "-game -carla-rpc-port=2000"
        )
        self.assertTrue(
            LAUNCHER.simulator_command_matches(command, editor, project, 2000)
        )
        self.assertFalse(
            LAUNCHER.simulator_command_matches(
                command, editor, Path("/work/Other.uproject"), 2000
            )
        )
        self.assertFalse(
            LAUNCHER.simulator_command_matches(command, editor, project, 2001)
        )
        self.assertFalse(
            LAUNCHER.simulator_command_matches(
                "/tmp/fake" + command, editor, project, 2000
            )
        )

    def test_adopt_reused_simulator_requires_one_verified_listener(self):
        arguments = type(
            "Arguments",
            (),
            {
                "unreal_editor": Path("/opt/UnrealEditor"),
                "uproject": Path("/work/CarlaUnreal.uproject"),
            },
        )()
        matching = (
            "/opt/UnrealEditor /work/CarlaUnreal.uproject "
            "-carla-rpc-port=2000"
        )
        with mock.patch.object(
            LAUNCHER, "listening_process_ids", return_value=[42, 43]
        ), mock.patch.object(
            LAUNCHER,
            "process_command",
            side_effect=lambda process_id: (
                matching if process_id == 42 else "other"
            ),
        ):
            self.assertEqual(LAUNCHER.adopt_reused_simulator(arguments, 2000), 42)

    def test_stop_reused_simulator_starts_with_interrupt(self):
        arguments = type(
            "Arguments",
            (),
            {
                "unreal_editor": Path("/opt/UnrealEditor"),
                "uproject": Path("/work/CarlaUnreal.uproject"),
            },
        )()
        matching = (
            "/opt/UnrealEditor /work/CarlaUnreal.uproject "
            "-carla-rpc-port=2000"
        )
        with mock.patch.object(
            LAUNCHER, "process_command", return_value=matching
        ), mock.patch.object(
            LAUNCHER, "wait_for_process_exit", return_value=True
        ), mock.patch.object(LAUNCHER.os, "kill") as kill:
            LAUNCHER.stop_reused_simulator(42, arguments, 2000)
        kill.assert_called_once_with(42, LAUNCHER.signal.SIGINT)

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

    def test_control_socket_uses_a_short_private_runtime_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            control_directory = Path(directory) / "c"
            socket_file, token_file = RUNNER.control_paths(control_directory)
            self.assertEqual(socket_file, control_directory / "control.sock")
            self.assertEqual(token_file, control_directory / "control.token")
            self.assertEqual(control_directory.stat().st_mode & 0o777, 0o700)
            self.assertLessEqual(
                len(os.fsencode(socket_file)),
                RUNNER.PORTABLE_UNIX_SOCKET_PATH_MAX,
            )

    def test_control_socket_rejects_an_overlong_runtime_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            control_directory = Path(directory) / ("x" * 90)
            with self.assertRaisesRegex(ValueError, "Unix-domain limit"):
                RUNNER.control_paths(control_directory)

    def test_m6_acceptance_uses_a_short_private_runtime_directory(self):
        control_directory, socket_file, token_file = M6_RUNNER.create_control_paths()
        try:
            self.assertEqual(socket_file, control_directory / "control.sock")
            self.assertEqual(token_file, control_directory / "control.token")
            self.assertEqual(control_directory.stat().st_mode & 0o777, 0o700)
            self.assertLessEqual(
                len(os.fsencode(socket_file)),
                M6_RUNNER.PORTABLE_UNIX_SOCKET_PATH_MAX,
            )
        finally:
            M6_RUNNER.shutil.rmtree(control_directory, ignore_errors=True)

    def test_manual_cleanup_requires_stopped_owner_and_removed_secrets(self):
        with tempfile.TemporaryDirectory() as directory:
            run_directory = Path(directory)
            control_directory = run_directory / "control"
            control_directory.mkdir()
            RUNNER.M5.atomic_write_json(
                run_directory / "controller-status.json",
                {
                    "state": "stopped",
                    "control": {"session_active": False, "owner": None},
                },
            )
            self.assertTrue(
                LAUNCHER.manual_run_cleanup_is_valid(
                    run_directory, control_directory
                )
            )
            (control_directory / "control.token").write_text("secret")
            self.assertFalse(
                LAUNCHER.manual_run_cleanup_is_valid(
                    run_directory, control_directory
                )
            )


if __name__ == "__main__":
    unittest.main()
