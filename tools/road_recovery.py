"""Read-only validation of the configured CARLA road recovery placement."""
from __future__ import annotations
import math


def placement_rejection(world, map_, carla, ego, candidate):
    """Conservative actor clearance at the known-good, lane-aligned spawn.

    The tick owner calls this; it never moves actors or advances simulation.
    Unknown geometry is not a free-space confirmation.
    """
    try:
        loc, rot = candidate.location, candidate.rotation
        if not all(math.isfinite(value) for value in (loc.x,loc.y,loc.z,rot.yaw,rot.pitch,rot.roll)):
            return "ROAD_POSITION_INVALID"
        lane = map_.get_waypoint(loc,project_to_road=True,lane_type=carla.LaneType.Driving)
        if lane is None or lane.lane_type != carla.LaneType.Driving:
            return "ROAD_POSITION_INVALID"
        heading = abs((rot.yaw-lane.transform.rotation.yaw+180)%360-180)
        if loc.distance(lane.transform.location)>2 or heading>10 or abs(rot.roll)>10 or abs(rot.pitch)>15:
            return "ROAD_POSITION_INVALID"
        extent=ego.bounding_box.extent
        if not all(math.isfinite(v) and v>0 for v in (extent.x,extent.y,extent.z)):
            return "ROAD_POSITION_UNCONFIRMED"
        radius=math.hypot(extent.x,extent.y)
        for actor in world.get_actors():
            if actor.id==ego.id or not actor.type_id.startswith(("vehicle.","walker.pedestrian.","static.prop.")):
                continue
            other=actor.get_transform().location
            box=actor.bounding_box.extent
            if not all(math.isfinite(v) for v in (other.x,other.y,other.z,box.x,box.y,box.z)) or min(box.x,box.y,box.z)<0:
                return "ROAD_POSITION_UNCONFIRMED"
            if abs(loc.z-other.z)>extent.z+box.z+1:
                continue
            if math.hypot(loc.x-other.x,loc.y-other.y)<=radius+math.hypot(box.x,box.y)+1:
                return "ROAD_POSITION_OCCUPIED"
        return None
    except (AttributeError,RuntimeError,TypeError,ValueError):
        return "ROAD_POSITION_UNCONFIRMED"
