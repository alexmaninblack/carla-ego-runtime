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

    class Vector3D(Vector):
        pass


class HandoverTests(unittest.TestCase):
    def test_repeated_brake_after_traffic_manager_is_not_cached_away(self):
        class Blueprint:
            values = {"role_name": "", "sticky_control": "true"}

            def has_attribute(self, name):
                return name in self.values

            def set_attribute(self, name, value):
                self.values[name] = value

        blueprint = Blueprint()
        CONTROLLER.configure_ego_blueprint(blueprint, "hero")
        self.assertEqual(blueprint.values["role_name"], "hero")
        self.assertEqual(blueprint.values["sticky_control"], "false")
        # Reproduce LibCarla/client/Vehicle.cpp's independent command cache:
        # this proxy last sent full brake, TM then sent throttle via its batch.
        cached = (0.0, 1.0)
        physical = (0.4, 0.0)
        requested = (0.0, 1.0)
        if blueprint.values["sticky_control"] != "true" or requested != cached:
            physical = requested
        self.assertEqual(physical, (0.0, 1.0))

    def test_missing_control_attribute_is_rejected_before_spawn(self):
        blueprint = type("Blueprint", (), {"has_attribute": lambda _, name: name == "role_name"})()
        with self.assertRaisesRegex(RuntimeError, "control attributes"):
            CONTROLLER.configure_ego_blueprint(blueprint, "hero")
        with self.assertRaises(RuntimeError):
            CONTROLLER.configure_ego_blueprint(None, "hero")

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

    def test_scenario_metrics_never_serialize_infinity(self):
        metrics = CONTROLLER.bounded_scenario_metrics(
            0, 0.0, None, 0.0, math.inf, None, []
        )
        self.assertIsNone(metrics["minimum_obstacle_gap_m"])
        self.assertEqual(metrics["collision_count"], 0)


if __name__ == "__main__":
    unittest.main()
