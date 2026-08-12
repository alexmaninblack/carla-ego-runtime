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


CONTROLLER = load_module(
    "m62_controller_tested", REPOSITORY / "tools" / "external_control_controller.py"
)


class Vector:
    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.x = x
        self.y = y
        self.z = z


class Location(Vector):
    def distance(self, other):
        return math.sqrt(
            (self.x - other.x) ** 2
            + (self.y - other.y) ** 2
            + (self.z - other.z) ** 2
        )


class Transform:
    def __init__(self, location, forward):
        self.location = location
        self.forward = forward

    def get_forward_vector(self):
        return self.forward


class Vehicle:
    def __init__(self, transform):
        self.transform = transform

    def get_transform(self):
        return self.transform


class Map:
    def __init__(self, waypoint):
        self.waypoint = waypoint

    def get_waypoint(self, *_args, **_kwargs):
        return self.waypoint


class Carla:
    class LaneType:
        Driving = "driving"

    class VehicleControl:
        def __init__(self, throttle=0.0, brake=0.0, steer=0.0):
            self.throttle = throttle
            self.brake = brake
            self.steer = steer


class HandoverTests(unittest.TestCase):
    def test_manual_handover_blends_without_overlapping_pedals(self):
        automatic = Carla.VehicleControl(throttle=0.4, brake=0.0, steer=0.3)
        manual = Carla.VehicleControl(throttle=0.0, brake=0.6, steer=-0.1)
        blended = CONTROLLER.blend_control(Carla, automatic, manual, 0.5)
        self.assertEqual(blended.throttle, 0.0)
        self.assertAlmostEqual(blended.brake, 0.1)
        self.assertAlmostEqual(blended.steer, 0.1)

    def test_manual_handover_clamps_to_both_endpoints(self):
        automatic = Carla.VehicleControl(throttle=0.4, brake=0.0, steer=0.3)
        manual = Carla.VehicleControl(throttle=0.0, brake=0.6, steer=-0.1)
        first = CONTROLLER.blend_control(Carla, automatic, manual, -1.0)
        second = CONTROLLER.blend_control(Carla, automatic, manual, 2.0)
        self.assertAlmostEqual(first.throttle, automatic.throttle)
        self.assertAlmostEqual(first.steer, automatic.steer)
        self.assertAlmostEqual(second.brake, manual.brake)
        self.assertAlmostEqual(second.steer, manual.steer)

    def test_autopilot_requires_a_nearby_forward_driving_lane(self):
        vehicle = Vehicle(Transform(Location(0, 0, 0), Vector(1, 0, 0)))
        aligned = type(
            "Waypoint",
            (),
            {"transform": Transform(Location(1, 0, 0), Vector(1, 0, 0))},
        )()
        self.assertIsNone(
            CONTROLLER.autopilot_unavailable_reason(
                vehicle, Map(aligned), Carla, 2.5, 60
            )
        )
        far = type(
            "Waypoint",
            (),
            {"transform": Transform(Location(3, 0, 0), Vector(1, 0, 0))},
        )()
        self.assertIn(
            "away from the road",
            CONTROLLER.autopilot_unavailable_reason(
                vehicle, Map(far), Carla, 2.5, 60
            ),
        )
        opposite = type(
            "Waypoint",
            (),
            {"transform": Transform(Location(1, 0, 0), Vector(-1, 0, 0))},
        )()
        self.assertIn(
            "against the driving lane",
            CONTROLLER.autopilot_unavailable_reason(
                vehicle, Map(opposite), Carla, 2.5, 60
            ),
        )
        self.assertIn(
            "away from a driving lane",
            CONTROLLER.autopilot_unavailable_reason(
                vehicle, Map(None), Carla, 2.5, 60
            ),
        )


if __name__ == "__main__":
    unittest.main()
