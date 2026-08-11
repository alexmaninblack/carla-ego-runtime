# Draft telemetry contract v0

Status: **planning draft**. The semantic contract is being fixed before a wire
format or transport is selected.

## Common envelope

Every message carries the following metadata:

| Field | Type | Meaning |
| --- | --- | --- |
| `schema_version` | string | Initially `0.1` |
| `run_id` | string | Identifies one simulator run |
| `vehicle_id` | string | Stable ego-vehicle identifier within the run |
| `frame_id` | unsigned integer | CARLA simulation frame |
| `sim_time_s` | double | Simulation time in seconds |
| `sequence` | unsigned integer | Monotonic sequence for this stream |
| `coordinate_frame` | string | Explicit coordinate-system identifier |

Wall-clock time is optional diagnostic metadata and must not replace simulation
time for ordering or synchronization.

## VehicleState v0

Nominal rate: **20 Hz**, once per simulation frame.

| Field | Unit/range | Source and meaning |
| --- | --- | --- |
| `speed_mps` | m/s | Signed velocity projected onto the vehicle forward axis |
| `acceleration_mps2.x/y/z` | m/s² | Physical world-frame acceleration |
| `longitudinal_acceleration_mps2` | m/s² | Acceleration projected onto the vehicle forward axis |
| `throttle` | `0..1` | Last-applied accelerator command, not physical acceleration |
| `brake` | `0..1` | Last-applied brake command, not brake-system pressure |
| `steering_command` | `-1..1` | Last-applied normalized steering command |
| `front_left_wheel_angle_deg` | degrees | Actual steer angle of the front-left road wheel |
| `front_right_wheel_angle_deg` | degrees | Actual steer angle of the front-right road wheel |
| `gear` | integer | Current transmission gear |
| `engine_rpm` | rpm | Current simulated engine speed when available |

CARLA does not directly expose a physical steering-wheel rotation angle. The
first contract therefore exposes both the normalized steering command and the
two front road-wheel angles. A steering-wheel angle may be added later as a
clearly marked derived value with a configured steering ratio.

## GnssFix v0

Nominal rate: **10 Hz**.

| Field | Unit | Meaning |
| --- | --- | --- |
| `latitude_deg` | degrees | WGS84-like latitude reported by CARLA GNSS |
| `longitude_deg` | degrees | WGS84-like longitude reported by CARLA GNSS |
| `altitude_m` | metres | Altitude reported by CARLA GNSS |

The GNSS message uses the measurement frame and simulation timestamp supplied
by CARLA. Heading is not part of the initial GNSS contract.

## Coordinate convention

Until an external consumer requires a standard robotics frame, vector values
remain in an explicitly labelled CARLA coordinate frame (x forward, y right,
z up where applicable). Any future ENU, NED, or ROS conversion must happen in
one documented adapter and must change `coordinate_frame` accordingly.

## Not included in v0

Images, point clouds, radar detections, ultrasonic ranges, lane/collision
events, vehicle-control input from the network, and a specific serialization
technology are intentionally deferred.
