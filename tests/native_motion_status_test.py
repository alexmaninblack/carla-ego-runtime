"""Execute the production native telemetry view without a window or live input.

Synthetic fixtures stay inside this isolated process; no Gateway, CARLA or
service telemetry is replaced. Passing is a projection proof, not live QA.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"),
                     "macOS Swift required")
class NativeMotionStatusTests(unittest.TestCase):
    def test_speed_and_motion_use_the_same_accepted_sample(self):
        source = (ROOT / "tools/KeyboardControl.swift").read_text()
        view = "final class TelemetryView:" + source.split(
            "final class TelemetryView:", 1)[1].split(
            "final class DrivingWorkspace:", 1)[0]
        # Expose existing projections only in the test copy, without changing
        # their implementation or the production native application's API.
        view = view.replace("private func value(", "func value(")
        view = view.replace("private var motionText:", "var motionText:")
        checks = r'''
let view = TelemetryView(frame: NSRect(x: 0, y: 0, width: 500, height: 470))
func sample(_ speed: String?, _ mode: String = "SCENARIO",
            _ state: String = "LIVE", reset: Bool = false) -> [String: Any] {
    var signals = ["Vehicle.CarlaSimulation.Control.ActiveMode": mode,
                   "Vehicle.CarlaSimulation.Reset.InProgress": reset ? "true" : "false",
                   "Vehicle.CarlaSimulation.Reset.Discontinuity": "false"]
    signals["Vehicle.Speed"] = speed
    return ["schemaVersion": 1, "source": "gateway-viss", "state": state,
            "signals": signals]
}
assert(view.motionText == "NO FRESH DATA")
for _ in 0..<1000 {
    for mode in ["SCENARIO", "AUTOPILOT", "MANUAL", "SAFE_STOP"] {
        for (raw, expected) in [("0.0", "STOPPED"), ("0.3", "STOPPED"),
                                ("0.31", mode == "SAFE_STOP" ? "STOPPING" : "MOVING"),
                                ("19.7", mode == "SAFE_STOP" ? "STOPPING" : "MOVING"),
                                ("30.0", mode == "SAFE_STOP" ? "STOPPING" : "MOVING")] {
            view.accept(sample(raw, mode))
            assert(view.motionText == expected, "speed \(raw), mode \(mode): \(view.motionText)")
            assert(view.value("Speed") == String(format: "%.1f", Double(raw)!))
        }
    }
}
for raw: String? in [nil, "", "invalid", "nan", "inf", "-inf"] {
    view.accept(sample(raw))
    assert(view.motionText == "MOTION UNKNOWN")
    assert(view.value("Speed") == "—")
}
for state in ["WAITING", "STALE", "DISCONNECTED"] {
    view.accept(sample("30.0", "SCENARIO", state))
    assert(view.motionText == "NO FRESH DATA")
}
view.accept(sample("30.0", reset: true))
assert(view.motionText == "MOTION UNKNOWN")
var discontinuity = sample("30.0")
var signals = discontinuity["signals"] as! [String: String]
signals["Vehicle.CarlaSimulation.Reset.Discontinuity"] = "true"
discontinuity["signals"] = signals
view.accept(discontinuity)
assert(view.motionText == "MOTION UNKNOWN")
view.accept(sample("30.0"))
view.receivedAt = ProcessInfo.processInfo.systemUptime - 6
assert(view.motionText == "NO FRESH DATA")
view.accept(sample("30.0"))
var rejected = sample("0.0")
rejected["source"] = "wrong-source"
view.accept(rejected)
assert(view.value("Speed") == "30.0" && view.motionText == "MOVING")
print("PASS 20000 sample transitions and missing/reset/stale/invalid-input cases")
'''
        with tempfile.TemporaryDirectory(prefix="native-motion-status-") as temp:
            temp = Path(temp)
            harness = temp / "main.swift"
            harness.write_text(
                "import AppKit\nimport Foundation\nimport Darwin\n"
                "func jsonLine(_ event: String, fields: [String: Any] = [:]) {}\n"
                + view + checks)
            binary = temp / "motion-test"
            compiled = subprocess.run(
                ["xcrun", "swiftc", "-O", "-module-cache-path", str(temp / "cache"),
                 str(harness), "-o", str(binary), "-framework", "AppKit"],
                text=True, capture_output=True, timeout=90)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS 20000 sample transitions", result.stdout)


if __name__ == "__main__":
    unittest.main()
