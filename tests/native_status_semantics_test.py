"""Source-contract regressions; not a live CARLA qualification."""
import unittest
from pathlib import Path


class NativeStatusSemanticsTests(unittest.TestCase):
    def test_control_mode_does_not_claim_physical_movement(self):
        source = (Path(__file__).resolve().parents[1] / "tools/KeyboardControl.swift").read_text()
        self.assertIn("AUTOPILOT — MODE SELECTED", source)
        self.assertNotIn("AUTOPILOT — VEHICLE DRIVING", source)

    def test_link_display_and_click_use_same_fifteen_second_budget(self):
        source = (Path(__file__).resolve().parents[1] / "tools/KeyboardControl.swift").read_text()
        self.assertIn("systemUptime - externalReadAt <= 15", source)
        self.assertIn("systemUptime - connectivityReadAt <= 15", source)
        self.assertIn("EXTERNAL NETWORK: STALE · CHECK", source)
        self.assertIn("self.view.externalReadAt = self.connectivityReadAt", source)

    def test_advisory_uses_endpoint_recommendations_not_a_cloud_shortcut(self):
        source = (Path(__file__).resolve().parents[1] / "tools/KeyboardControl.swift").read_text()
        self.assertIn('"TIRE_INSPECTION_RECOMMENDED": "Inspection recommended"', source)
        self.assertIn('"TIRE_REPLACEMENT_RECOMMENDED": "Replacement recommended"', source)
        self.assertIn('"EXPIRED": "Expired"', source)
        self.assertNotIn('"SERVICE_REQUIRED": "Service required"', source)


if __name__ == "__main__":
    unittest.main()
