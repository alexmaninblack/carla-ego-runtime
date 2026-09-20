"""Physical placement checks; no simulator, network or vehicle mutation."""
import math
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace as N

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.road_recovery import placement_rejection

def location(x=0, y=0, z=0):
    return N(x=x, y=y, z=z, distance=lambda b: math.dist((x,y,z),(b.x,b.y,b.z)))

def transform(x=0, yaw=0):
    return N(location=location(x), rotation=N(yaw=yaw, pitch=0, roll=0))

def actor(identity=1, x=0, kind="vehicle.fixture"):
    return N(id=identity, type_id=kind, bounding_box=N(extent=N(x=2,y=1,z=1)),
             get_transform=lambda: transform(x))

class PlacementTests(unittest.TestCase):
    def check(self, others=(), waypoint=None, candidate=None):
        carla=N(LaneType=N(Driving=1))
        lane=waypoint if waypoint is not None else N(transform=transform(), lane_type=1)
        world=N(get_actors=lambda:[actor()]+list(others))
        map_=N(get_waypoint=lambda *args, **kwargs:lane)
        return placement_rejection(world,map_,carla,actor(),candidate or transform())

    def test_known_spawn_is_lane_aligned_and_free(self):
        self.assertIsNone(self.check())
        self.assertIsNone(self.check([actor(2,20)]))

    def test_blocked_spawn_vehicle_walker_and_prop(self):
        for kind in ("vehicle.fixture","walker.pedestrian.fixture","static.prop.fixture"):
            self.assertEqual("ROAD_POSITION_OCCUPIED",self.check([actor(2,2,kind)]))

    def test_non_drivable_or_wrong_heading_rejected(self):
        self.assertEqual("ROAD_POSITION_INVALID",self.check(waypoint=N(transform=transform(),lane_type=2)))
        self.assertEqual("ROAD_POSITION_INVALID",self.check(candidate=transform(20)))
        self.assertEqual("ROAD_POSITION_INVALID",self.check(candidate=transform(yaw=180)))

    def test_nonfinite_and_failed_reads_are_not_clear_placement(self):
        self.assertEqual("ROAD_POSITION_INVALID",self.check(candidate=transform(float("nan"))))
        self.assertEqual("ROAD_POSITION_UNCONFIRMED",self.check([N(id=2,type_id="vehicle.bad")]))

if __name__=="__main__":unittest.main()
