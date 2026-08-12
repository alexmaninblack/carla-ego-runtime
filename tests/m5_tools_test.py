import importlib.util
import json
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


CONTROLLER = load_module(
    "m5_controller_tested", REPOSITORY / "tools" / "behavior_agent_controller.py"
)
RUNNER = load_module("m5_runner_tested", REPOSITORY / "tools" / "run_m5.py")
LAUNCHER = load_module("m5_launcher_tested", REPOSITORY / "tools" / "launch_m5.py")
M6_RUNNER = load_module("m6_runner_tested", REPOSITORY / "tools" / "run_m6.py")


class M5ToolTests(unittest.TestCase):
    def setUp(self):
        self.config_path = REPOSITORY / "config" / "m5_town10hd_route.json"
        self.config = CONTROLLER.load_config(self.config_path)

    def test_checked_in_configuration_is_valid_and_repeatable(self):
        self.assertEqual(self.config["controller"]["type"], "behavior_agent")
        self.assertEqual(self.config["route"]["start_spawn_point"], 40)
        self.assertEqual(self.config["route"]["destination_spawn_points"], [0, 40])
        self.assertEqual(len(self.config["carla"]["source_commit"]), 40)
        self.assertAlmostEqual(
            self.config["simulation"]["fixed_delta_seconds"], 1.0 / 30.0
        )
        self.assertEqual(self.config["runtime"]["log_every_frames"], 30)
        self.assertEqual(self.config["runtime"]["dashboard_period_ms"], 250)

    def test_checked_in_m6_configuration_is_safe_and_bounded(self):
        m6 = CONTROLLER.load_config(
            REPOSITORY / "config" / "m6_town10hd_external_control.json"
        )
        external = m6["controller"]["external_control"]
        self.assertEqual(m6["controller"]["type"], "external_control")
        self.assertLess(
            external["command_timeout_seconds"],
            external["ownership_timeout_seconds"],
        )
        self.assertLessEqual(external["command_timeout_seconds"], 0.25)
        self.assertLessEqual(external["maximum_session_seconds"], 60)

    def test_m6_motion_acceptance_requires_movement_and_final_stop(self):
        accepted = {
            "motion": {
                "total_distance_m": 12.0,
                "maximum_speed_kmh": 18.0,
                "current_speed_kmh": 0.1,
            }
        }
        self.assertTrue(M6_RUNNER.motion_is_verified(accepted))
        accepted["motion"]["current_speed_kmh"] = 2.0
        self.assertFalse(M6_RUNNER.motion_is_verified(accepted))
        accepted["motion"]["current_speed_kmh"] = 0.1
        accepted["motion"]["total_distance_m"] = 1.0
        self.assertFalse(M6_RUNNER.motion_is_verified(accepted))

    def test_behavior_agent_controller_refuses_external_control_live_mode(self):
        source = (REPOSITORY / "tools" / "behavior_agent_controller.py").read_text()
        self.assertIn(
            "controller.type must be behavior_agent for this controller", source
        )

    def test_behavior_agent_pid_uses_the_physics_interval(self):
        options = CONTROLLER.behavior_agent_options(self.config)
        self.assertAlmostEqual(
            options["dt"], self.config["simulation"]["fixed_delta_seconds"]
        )

    def test_tick_pacer_keeps_normal_deadline(self):
        self.assertEqual(
            CONTROLLER.resynchronize_tick_deadline(10.0, 10.01, 1.0 / 30.0),
            10.0,
        )

    def test_tick_pacer_drops_a_full_period_of_lag(self):
        self.assertEqual(
            CONTROLLER.resynchronize_tick_deadline(10.0, 10.04, 1.0 / 30.0),
            10.04,
        )

    def test_invalid_control_source_is_rejected(self):
        invalid = json.loads(json.dumps(self.config))
        invalid["controller"]["type"] = "implicit"
        with self.assertRaisesRegex(
            CONTROLLER.ConfigurationError, "behavior_agent or external_control"
        ):
            CONTROLLER.validate_config(invalid)

    def test_route_requires_at_least_one_destination(self):
        invalid = json.loads(json.dumps(self.config))
        invalid["route"]["destination_spawn_points"] = []
        with self.assertRaises(CONTROLLER.ConfigurationError):
            CONTROLLER.validate_config(invalid)

    def test_status_write_is_atomic_and_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "status.json"
            CONTROLLER.atomic_write_json(path, {"state": "ready"})
            self.assertEqual(json.loads(path.read_text())["state"], "ready")
            self.assertFalse(path.with_suffix(".json.tmp").exists())

    def test_controller_stop_state_is_distinct_from_failure(self):
        source = (REPOSITORY / "tools" / "behavior_agent_controller.py").read_text()
        self.assertIn('"completed" if completed else "stopped" if stopped else "failed"', source)

    def test_runtime_manifest_redacts_tls_paths(self):
        command = RUNNER.runtime_command(
            Path("runtime"), self.config, Path("secret-cert.pem"), Path("secret-key.pem"), True
        )
        public = RUNNER.public_runtime_options(command)
        text = " ".join(public)
        self.assertNotIn("secret-cert.pem", text)
        self.assertNotIn("secret-key.pem", text)
        self.assertIn("<redacted-local-path>", text)
        self.assertIn("--observe-ticks", public)
        self.assertNotIn("--autopilot", public)

    def test_runtime_is_a_non_owning_observer(self):
        command = RUNNER.runtime_command(
            Path("runtime"), self.config, Path("cert.pem"), Path("key.pem"), False
        )
        self.assertIn("--no-spawn", command)
        self.assertIn("--observe-ticks", command)
        self.assertNotIn("--real-time", command)

    def test_dashboard_uses_configured_health_period(self):
        command = RUNNER.dashboard_command(
            Path("client"), self.config, Path("certificate.pem")
        )
        self.assertIn("--monitor", command)
        period_index = command.index("--monitor-period-ms")
        self.assertEqual(command[period_index + 1], "250")

    def test_dashboard_health_lines_become_manifest_metrics(self):
        health = {}
        RUNNER.collect_dashboard_health("Simulation rate   30.0 Hz", health)
        RUNNER.collect_dashboard_health("Dashboard rate    4.0 events/s", health)
        RUNNER.collect_dashboard_health("VISS latency      0.8 ms (local)", health)
        RUNNER.collect_dashboard_health("Events received   42", health)
        self.assertEqual(health["simulation_hz"], 30.0)
        self.assertEqual(health["delivery_hz"], 4.0)
        self.assertEqual(health["event_latency_ms"], 0.8)
        self.assertEqual(health["events_received"], 42)

    def test_dashboard_health_strips_terminal_escape_sequences(self):
        health = {}
        RUNNER.collect_dashboard_health(
            "\x1b[2J\x1b[HConnection        CONNECTED", health
        )
        self.assertEqual(health["connection"], "CONNECTED")

    def test_launcher_starts_integrated_dashboard_session(self):
        arguments = type("Arguments", (), {})()
        arguments.python = Path("python")
        arguments.config = Path("config.json")
        arguments.runtime = Path("runtime")
        arguments.viss_client = Path("client")
        arguments.python_api_root = Path("python-api")
        arguments.certificate = Path("certificate.pem")
        arguments.private_key = Path("private-key.pem")
        arguments.run_root = Path("runs")
        arguments.route_cycles = None
        arguments.maximum_route_seconds = None
        arguments.dashboard_quiet = False
        command = LAUNCHER.orchestrator_command(arguments)
        self.assertIn("--dashboard", command)
        self.assertIn("run_m5.py", " ".join(command))

    def test_endurance_override_scales_route_timeout(self):
        effective = RUNNER.apply_run_overrides(self.config, 14)
        self.assertEqual(effective["route"]["cycles"], 14)
        self.assertEqual(effective["simulation"]["maximum_route_seconds"], 5040)

    def test_launcher_lock_replaces_stale_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "session.lock"
            path.write_text("999999999\n")
            lock = LAUNCHER.SessionLock(path)
            lock.acquire()
            self.assertEqual(int(path.read_text()), os.getpid())
            lock.release()
            self.assertFalse(path.exists())

    def test_launcher_isolates_owned_process_signal_groups(self):
        source = (REPOSITORY / "tools" / "launch_m5.py").read_text()
        self.assertGreaterEqual(source.count("start_new_session=True"), 2)

    def test_live_runner_requires_independent_viss_client(self):
        arguments = type("Arguments", (), {})()
        arguments.runtime = Path("missing-runtime")
        arguments.viss_client = None
        arguments.python_api_root = Path("missing-api")
        arguments.certificate = Path("missing-cert")
        arguments.private_key = Path("missing-key")
        arguments.repeat = 1
        with self.assertRaisesRegex(ValueError, "--runtime|--viss-client"):
            RUNNER.require_live_paths(arguments)

    def test_m5_dependencies_are_exactly_pinned(self):
        requirements = (REPOSITORY / "tools" / "requirements-m5.txt").read_text()
        packages = [line for line in requirements.splitlines() if line and not line.startswith("#")]
        self.assertTrue(packages)
        self.assertTrue(all("==" in package for package in packages))


if __name__ == "__main__":
    unittest.main()
