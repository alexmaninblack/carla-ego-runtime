# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: MIT
"""Production native semantic/dispatch unit gates; never launch a UI window."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AccessibilitySourceTests(unittest.TestCase):
    def test_virtual_buttons_share_layout_and_guarded_dispatch(self):
        source = (ROOT / 'tools/KeyboardControl.swift').read_text()
        self.assertIn('NSAccessibility.screenRect(fromView: control, rect: control.buttonRect(button))', source)
        self.assertIn('control?.activate(button) ?? false', source)
        self.assertIn('buttonRect($0).contains(point)', source)
        self.assertIn('activate(button)', source.split('override func mouseDown', 1)[1])
        self.assertIn('override func accessibilityHitTest', source)
        self.assertIn('override func isAccessibilityEnabled()', source)
        self.assertIn('window.setAccessibilityIdentifier("driving.window")', source)
        self.assertIn('$0.presented(hasScenes: onScene != nil) && buttonRect($0).contains(point)', source)

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'macOS Swift required')
    def test_exact_ids_and_existing_enabled_guards(self):
        source = (ROOT / 'tools/KeyboardControl.swift').read_text()
        semantic = 'enum DrivingButton:' + source.split('enum DrivingButton:', 1)[1].split('// End stable native UI semantics.', 1)[0]
        assertions = '''
let expected = Set(["driving.safe-stop", "driving.autopilot", "driving.brake-maneuver", "driving.tire-maneuver", "driving.return-to-road"])
assert(Set(DrivingButton.allCases.map(\\.rawValue)) == expected)
assert(DrivingButton.allCases.count == 5)
assert(DrivingButton(rawValue: "driving.delete-test") == nil)
for busy in [false, true] {
    for scenes in [false, true] {
        assert(DrivingButton.safeStop.enabled(sceneBusy: busy, hasScenes: scenes))
        assert(DrivingButton.autopilot.enabled(sceneBusy: busy, hasScenes: scenes) == !busy)
        assert(DrivingButton.safeStop.presented(hasScenes: scenes))
        assert(DrivingButton.autopilot.presented(hasScenes: scenes))
        for action in [DrivingButton.brake, .tire, .road] {
            assert(action.enabled(sceneBusy: busy, hasScenes: scenes) == (scenes && !busy))
            assert(action.presented(hasScenes: scenes) == scenes)
        }
    }
}
assert(Set(DrivingButton.allCases.map(\\.label)).count == 5)
print("PASS stable Accessibility semantics")
'''
        with tempfile.TemporaryDirectory(prefix='native-accessibility-unit-') as directory:
            path = Path(directory) / 'main.swift'
            path.write_text('import Foundation\n' + semantic + assertions)
            result = subprocess.run(['xcrun', 'swift', '-module-cache-path', str(Path(directory) / 'cache'), str(path)],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('PASS stable Accessibility semantics', result.stdout)


if __name__ == '__main__':
    unittest.main()
