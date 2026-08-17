#!/usr/bin/env python3
"""Dependency-free tests for the deterministic CARLA braking scenario."""

# Copyright (c) 2026 maninblack
# SPDX-License-Identifier: MIT

import importlib.util
import math
import sys
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]


def load_module(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


SCENARIO = load_module(
    "brake_event_scenario_tested",
    REPOSITORY / "tools" / "brake_event_scenario.py",
)


class Vector:
    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.x = x
        self.y = y
        self.z = z


class Transform:
    def __init__(self, location, forward, right=None):
        self.location = location
        self.forward = forward
        self.right = right or Vector(-forward.y, forward.x, 0.0)

    def get_forward_vector(self):
        return self.forward

    def get_right_vector(self):
        return self.right


class Waypoint:
    def __init__(self, road_id, lane_id, heading, candidates=None):
        self.road_id = road_id
        self.lane_id = lane_id
        self.s = 0.0
        self.transform = Transform(Vector(), heading)
        self.candidates = candidates or []

    def next(self, _distance):
        return self.candidates


class BrakeEventScenarioTests(unittest.TestCase):
    def setUp(self):
        self.config = SCENARIO.M5.load_config(
            REPOSITORY / "config" / "brake_event_town10hd.json"
        )
        self.scenario = self.config["controller"]["scenario"]

    def test_checked_in_configuration_is_valid(self):
        self.assertEqual(
            self.config["controller"]["type"], "brake_event_scenario"
        )
        self.assertEqual(self.config["route"]["start_spawn_point"], 40)
        self.assertAlmostEqual(
            self.config["simulation"]["fixed_delta_seconds"], 1.0 / 30.0
        )
        self.assertGreater(
            self.scenario["brake_trigger_gap_m"],
            self.scenario["minimum_stop_gap_m"],
        )

    def test_branch_choice_prefers_heading_alignment_then_stable_ids(self):
        straight = Waypoint(20, 1, Vector(1.0, 0.0, 0.0))
        left = Waypoint(10, 1, Vector(0.0, -1.0, 0.0))
        current = Waypoint(
            1, 1, Vector(1.0, 0.0, 0.0), candidates=[left, straight]
        )
        self.assertIs(SCENARIO.choose_forward_waypoint(current, 2.0), straight)

    def test_steering_uses_vehicle_right_axis_and_is_bounded(self):
        transform = Transform(
            Vector(0.0, 0.0, 0.0),
            Vector(1.0, 0.0, 0.0),
            Vector(0.0, 1.0, 0.0),
        )
        self.assertGreater(
            SCENARIO.steering_command(transform, Vector(5.0, 2.0, 0.0), 0.45),
            0.0,
        )
        self.assertEqual(
            SCENARIO.steering_command(transform, Vector(0.0, 5.0, 0.0), 0.45),
            0.45,
        )

    def test_state_machine_reaches_brake_hold_and_complete(self):
        scenario = dict(self.scenario)
        scenario["stabilization_seconds"] = 0.2
        scenario["hold_seconds"] = 0.2
        scenario["stopped_frames"] = 2
        machine = SCENARIO.BrakeScenarioStateMachine(scenario, 0.1)

        control = machine.step(0.0, 50.0)
        self.assertEqual(control.phase, "ACCELERATE")
        control = machine.step(20.0, 40.0)
        self.assertEqual(control.phase, "STABILIZE")
        machine.step(20.0, 35.0)
        control = machine.step(20.0, 30.0)
        self.assertEqual(control.phase, "APPROACH")
        control = machine.step(20.0, 12.5)
        self.assertEqual(control.phase, "BRAKE")
        self.assertEqual(control.brake, scenario["brake_command"])
        machine.step(0.2, 6.0)
        control = machine.step(0.2, 6.0)
        self.assertEqual(control.phase, "HOLD")
        machine.step(0.0, 6.0)
        control = machine.step(0.0, 6.0)
        self.assertTrue(control.completed)
        self.assertEqual(machine.brake_onset_speed_kmh, 20.0)

    def test_result_evaluation_requires_safe_repeatable_stop(self):
        metrics = {
            "collision_count": 0,
            "stop_gap_m": 6.0,
            "brake_onset_speed_kmh": 20.0,
            "peak_deceleration_mps2": -5.0,
        }
        passed, reasons = SCENARIO.evaluate_result(self.scenario, metrics)
        self.assertTrue(passed)
        self.assertEqual(reasons, [])
        metrics["collision_count"] = 1
        metrics["stop_gap_m"] = 0.0
        passed, reasons = SCENARIO.evaluate_result(self.scenario, metrics)
        self.assertFalse(passed)
        self.assertGreaterEqual(len(reasons), 2)


if __name__ == "__main__":
    unittest.main()
