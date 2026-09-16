# SPDX-FileCopyrightText: 2026 maninblack
# SPDX-License-Identifier: MIT
import math
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.brake_event_scenario import QualificationManeuver


class ManeuverTests(unittest.TestCase):
    def test_brake_reuses_staged_scenario_without_fabricating_samples(self):
        maneuver = QualificationManeuver("brake", .05)
        self.assertGreater(maneuver.step(0, 0)[0].throttle, 0)
        self.assertEqual(.5, maneuver.step(30, 46)[0].brake)
        for _ in range(30): maneuver.step(20, 50)
        for _ in range(70): result, offset = maneuver.step(0, 60)
        self.assertTrue(result.completed)
        self.assertEqual(0, offset)
        self.assertEqual(31, maneuver.metrics()["brakingFramesAbove10Kmh"])

    def test_tire_inputs_are_bounded_and_end_stopped(self):
        maneuver = QualificationManeuver("tire", .05)
        offsets = []
        for _ in range(620):
            control, offset = maneuver.step(0, 0)
            self.assertTrue(0 <= control.throttle <= .5)
            self.assertTrue(0 <= control.brake <= 1)
            self.assertLessEqual(abs(offset), .5)
            offsets.append(offset)
        self.assertTrue(control.completed)
        self.assertGreater(max(offsets), .49)
        self.assertLess(min(offsets), -.49)

    def test_invalid_observations_and_period_fail_closed(self):
        for period in (0, -1, math.nan, math.inf, .2):
            with self.assertRaises(ValueError): QualificationManeuver("brake", period)
        maneuver = QualificationManeuver("tire", .05)
        for speed, distance in ((math.nan, 0), (0, math.inf), (-1, 0), (0, -1)):
            with self.assertRaises(ValueError): maneuver.step(speed, distance)
