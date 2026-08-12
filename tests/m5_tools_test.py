import importlib.util
import json
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


class M5ToolTests(unittest.TestCase):
    def setUp(self):
        self.config_path = REPOSITORY / "config" / "m5_town10hd_route.json"
        self.config = CONTROLLER.load_config(self.config_path)

    def test_checked_in_configuration_is_valid_and_repeatable(self):
        self.assertEqual(self.config["controller"]["type"], "behavior_agent")
        self.assertEqual(self.config["route"]["start_spawn_point"], 40)
        self.assertEqual(self.config["route"]["destination_spawn_points"], [0, 40])
        self.assertEqual(len(self.config["carla"]["source_commit"]), 40)

    def test_invalid_control_source_is_rejected(self):
        invalid = json.loads(json.dumps(self.config))
        invalid["controller"]["type"] = "implicit"
        with self.assertRaisesRegex(CONTROLLER.ConfigurationError, "behavior_agent"):
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
