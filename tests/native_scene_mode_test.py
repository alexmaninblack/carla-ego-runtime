"""Execute the production pure Swift projection, without launching native UI."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(sys.platform == "darwin" and shutil.which("xcrun"), "macOS Swift required")
class NativeSceneModeTests(unittest.TestCase):
    def test_confirmed_modes_and_uncertain_receipts(self):
        source = (ROOT / "tools/KeyboardControl.swift").read_text()
        projection = "func confirmedSceneMode(" + source.split("func confirmedSceneMode(", 1)[1].split("// End receipt projection.", 1)[0]
        assertions = r'''
func receipt(_ kind: String, _ state: String = "COMPLETED") -> [String: Any] {
    return ["operation": kind == "return_to_road" ? "simulation.return-to-road" : "simulation.exercise",
            "data": ["kind": kind, "state": state, "currentVehicle": "test", "physicalStop": "CONFIRMED",
                     "driveMode": "MANUAL", "autopilotStarted": false]]
}
let road = confirmedSceneMode(receipt("return_to_road"), kind: "return_to_road")!
assert(road.mode == "manual" && road.reason == "manual_ready")
for kind in ["brake", "tire"] {
    for state in ["COMPLETED", "ABORTED", "FAILED"] {
        let next = confirmedSceneMode(receipt(kind, state), kind: kind)!
        assert(next.mode == "safe_stop" && next.reason == "")
    }
}
assert(confirmedSceneMode(receipt("return_to_road", "FAILED"), kind: "return_to_road")?.mode == "safe_stop")
assert(confirmedSceneMode(nil, kind: "brake") == nil)
assert(confirmedSceneMode(receipt("tire"), kind: "brake") == nil)
for (key, value) in [("state", "RUNNING"), ("currentVehicle", "production"), ("physicalStop", "NOT_OBSERVED")] {
    var result = receipt("brake"); var data = result["data"] as! [String: Any]
    data[key] = value; result["data"] = data
    assert(confirmedSceneMode(result, kind: "brake") == nil)
}
for field in ["driveMode", "autopilotStarted"] {
    var result = receipt("return_to_road"); var data = result["data"] as! [String: Any]
    data.removeValue(forKey: field); result["data"] = data
    assert(confirmedSceneMode(result, kind: "return_to_road") == nil)
}
var foreign = receipt("brake"); foreign["operation"] = "simulation.other"
assert(confirmedSceneMode(foreign, kind: "brake") == nil)
print("PASS confirmed scene mode projection")
'''
        with tempfile.TemporaryDirectory(prefix="native-scene-mode-") as temp:
            harness = Path(temp) / "main.swift"
            harness.write_text("import Foundation\n" + projection + assertions)
            result = subprocess.run(["xcrun", "swift", "-module-cache-path", str(Path(temp) / "cache"), str(harness)],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS confirmed scene mode projection", result.stdout)

if __name__ == "__main__":
    unittest.main()
