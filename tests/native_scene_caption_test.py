"""Execute native caption geometry/state without starting a vehicle or window."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"), "macOS Swift required")
class NativeSceneCaptionTests(unittest.TestCase):
    def test_maneuver_copy_does_not_misidentify_tire_or_freeze_the_models(self):
        source = (ROOT / "tools/KeyboardControl.swift").read_text()
        self.assertIn("VEHICLE CONTROLLED BY THE ACTIVE TEST MANEUVER", source)
        self.assertNotIn("VEHICLE CONTROLLED BY THE BRAKE-EVENT STATE MACHINE", source)
        self.assertIn(r"Real \(kind) maneuver · no model Reset", source)
        self.assertNotIn("Preparing scene · real", source)
        self.assertNotIn("maneuver · models unchanged", source)

    def test_caption_stays_in_gap_and_does_not_claim_current_manual_mode(self):
        source = (ROOT / "tools/KeyboardControl.swift").read_text()
        prefix = source.split("final class TelemetryView:", 1)[0]
        assertions = r'''
let view = ControlView(frame: NSRect(x: 0, y: 0, width: 520, height: 600))
assert(view.sceneCaptionRect.minY > 122 && view.sceneCaptionRect.maxY < 138)
view.sceneDetail = "Return to road complete · models and advisory state retained."
view.setMode("manual", reason: "manual_ready")
view.setMode("autopilot")
assert(!view.sceneDetail.contains("stationary Manual"))
assert(view.mode == "autopilot")
view.setMode("safe_stop")
assert(view.mode == "safe_stop")
print("PASS scene caption geometry and historical feedback")
'''
        self.assertNotIn('"On road · stationary Manual · select Autopilot when ready."', source)
        self.assertIn("NSBezierPath(rect: sceneCaptionRect).addClip()", source)
        with tempfile.TemporaryDirectory(prefix="native-caption-") as temp:
            harness = Path(temp) / "main.swift"
            harness.write_text(prefix + assertions)
            result = subprocess.run(["xcrun", "swift", "-module-cache-path", str(Path(temp) / "cache"), str(harness)],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS scene caption", result.stdout)


if __name__ == "__main__":
    unittest.main()
