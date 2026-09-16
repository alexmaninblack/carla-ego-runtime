# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("m6_trust_under_test", ROOT / "tools/run_m6_interactive.py")
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


class TrustOptionsTest(unittest.TestCase):
    def test_development_stays_explicit_and_has_no_client_material(self):
        self.assertEqual((["--viss-development"], []), runner.viss_trust_options(
            argparse.Namespace(viss_development=True)))

    def test_missing_strict_configuration_never_falls_back(self):
        with self.assertRaisesRegex(ValueError, "strict VISS requires"):
            runner.viss_trust_options(argparse.Namespace(viss_development=False))

    def test_mixed_profiles_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "mixed"):
            runner.viss_trust_options(argparse.Namespace(viss_development=True,
                dashboard_private_key=Path("private")))

    def test_strict_forwards_one_dashboard_identity_for_probe_and_monitor(self):
        certificate = Mock()
        digest = hashlib.sha256(b"ephemeral certificate").hexdigest()
        args = argparse.Namespace(viss_development=False, viss_client_ca=Path("ca"),
            viss_assignment_socket=Path("assignment.sock"), viss_assignment_generation=7,
            dashboard_certificate=certificate, dashboard_private_key=Path("key"),
            dashboard_certificate_sha256=digest)
        with patch("ssl.PEM_cert_to_DER_cert", return_value=b"ephemeral certificate"):
            runtime, client = runner.viss_trust_options(args)
            self.assertIn("--viss-strict-client-auth", runtime)
            self.assertNotIn("--viss-development", runtime)
            self.assertEqual(["--cert", str(certificate), "--key", "key"], client)
            args.dashboard_certificate_sha256 = "0" * 64
            with self.assertRaisesRegex(ValueError, "does not match"):
                runner.viss_trust_options(args)


if __name__ == "__main__":
    unittest.main()
