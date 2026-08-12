# VISS/VSS telemetry contract v0.1

Status: **implemented for the M4 vehicle-state, GNSS, and VISS network subset**.

## Standards baseline

The external interface uses:

- COVESA Vehicle Information Service Specification (VISS) 3.1 for service,
  operations, payload, transport, errors, and subscriptions;
- COVESA Vehicle Signal Specification (VSS) 6.0 for paths, data types, units,
  and signal meaning;
- a small project-owned VSS overlay for CARLA simulation metadata that has no
  standard production-vehicle equivalent.

VISS is not a single telemetry envelope. A client reads or subscribes to paths
in a VSS tree, and the VISS response carries the path, value, timestamp, request
identifier, and, for events, subscription identifier. The detailed transport
subset is defined in the [VISS compatibility profile](viss-profile.md).

## Standard signal mapping

| VSS 6.0 path | VSS type and unit | CARLA source and conversion |
| --- | --- | --- |
| `Vehicle.Speed` | `float`, km/h | Non-negative velocity magnitude multiplied by 3.6. Direction is not encoded by making this value negative. |
| `Vehicle.Acceleration.Longitudinal` | `float`, m/s² | Physical acceleration transformed to the ISO 8855 vehicle X axis. |
| `Vehicle.Acceleration.Lateral` | `float`, m/s² | Physical acceleration transformed to the ISO 8855 vehicle Y axis. CARLA's Y-right convention must be converted to the VSS/ISO sign convention. |
| `Vehicle.Acceleration.Vertical` | `float`, m/s² | Physical acceleration transformed to the ISO 8855 vehicle Z axis. |
| `Vehicle.Chassis.Accelerator.PedalPosition` | `uint8`, percent | Last-applied CARLA throttle command multiplied by 100 and rounded deterministically. This represents a simulated pedal-position proxy, not measured pedal hardware. |
| `Vehicle.Chassis.Brake.PedalPosition` | `uint8`, percent | Last-applied CARLA brake command multiplied by 100 and rounded deterministically. This represents a simulated pedal-position proxy, not hydraulic pressure. |
| `Vehicle.Chassis.Axle.Row1.SteeringAngle` | `float`, degrees | Equivalent single-track front-axle angle derived from the actual left and right front road-wheel angles; positive left and negative right. |
| `Vehicle.Powertrain.Transmission.CurrentGear` | `int8` | CARLA current gear: zero neutral, positive forward, negative reverse. |
| `Vehicle.Powertrain.CombustionEngine.Speed` | `float`, rpm | CARLA simulated engine speed when the selected vehicle exposes it. Omitted or marked unavailable otherwise. |
| `Vehicle.CurrentLocation.Latitude` | `double`, degrees | CARLA GNSS latitude. |
| `Vehicle.CurrentLocation.Longitude` | `double`, degrees | CARLA GNSS longitude. |
| `Vehicle.CurrentLocation.Altitude` | `double`, m | CARLA GNSS altitude. |

All mandatory values are finite and range-checked before projection. CARLA
throttle and brake commands must be in `[0, 1]`; percentages use
`std::lround(command * 100)`, i.e. halfway values round away from zero. The
gear must fit the VSS `int8` range. A malformed mandatory value rejects the
complete frame instead of publishing a partially corrupt update.

## Steering semantics

CARLA's normalized steering command in `[-1, 1]` is useful internally for
debugging control, but it is not a physical steering-wheel or road-wheel angle.
It therefore has no standard VSS path in the initial external tree.

CARLA also does not directly expose physical steering-wheel rotation. The
runtime must not publish a fabricated value at
`Vehicle.Chassis.SteeringWheel.Angle`. The standard external signal is the
single-track equivalent at `Vehicle.Chassis.Axle.Row1.SteeringAngle`, derived
from the two front road-wheel angles. Raw commands or wheel angles can be added
later under a clearly named project overlay if a real consumer needs them.

CARLA/Unreal wheel angles are positive to the right, while VSS follows ISO
8855 and is positive to the left. Each road-wheel angle is therefore negated.
For valid, same-direction left and right front angles `δl` and `δr`, the
single-track equivalent is:

`δ = atan(2 / (cot(δl) + cot(δr)))`

The calculation is performed in radians and returned in degrees. Zero/zero
maps to zero. Non-finite angles, magnitudes of 90 degrees or more, and
contradictory non-zero signs make the external steering signal unavailable.

World-space acceleration is inverse-rotated by the ego transform. The CARLA
vehicle-axis result `(x forward, y right, z up)` becomes the ISO 8855 result
`(x, -y, z)`.

## CARLA simulation overlay

The following project-owned paths extend, but do not modify, VSS 6.0:

| Extension path | Type and unit | Meaning |
| --- | --- | --- |
| `Vehicle.CarlaSimulation.ProfileVersion` | `string` attribute | Version of this compatibility profile, initially `0.1`. |
| `Vehicle.CarlaSimulation.RunId` | `string` sensor | Unique identifier of one simulator run. |
| `Vehicle.CarlaSimulation.EgoVehicleId` | `string` sensor | Stable ego-vehicle identifier within the run. |
| `Vehicle.CarlaSimulation.FrameId` | `uint64` sensor | CARLA simulation frame represented by the sample. |
| `Vehicle.CarlaSimulation.SimulationTime` | `double`, s | Exact CARLA elapsed simulation time. |
| `Vehicle.CarlaSimulation.GnssFrameId` | `uint64` sensor | Source CARLA frame of the retained GNSS fix. |
| `Vehicle.CarlaSimulation.GnssSimulationTime` | `double`, s | Source CARLA simulation time of the retained GNSS fix. |

The versioned artifact is
[`vss/Vehicle.CarlaSimulation.vspec`](../vss/Vehicle.CarlaSimulation.vspec).
It is structurally validated in the dependency-free test suite and was also
merged with the complete VSS 6.0 catalog under `vss-tools` 6.0 `--strict`.
The merged tree confirms the documented types and units. The overlay uses a
project namespace and is not presented as part of standard COVESA VSS.

## Time and synchronization

VISS timestamps use ISO 8601 UTC with a trailing `Z`. At the first sample, the
runtime records a pair consisting of UTC time and current CARLA simulation
time. Later timestamps add the elapsed simulation-time difference to that UTC
anchor. This preserves monotonic simulated timing even when the simulator runs
slower or faster than real time or the runtime attaches after frame zero.

`Vehicle.CarlaSimulation.RunId`, `FrameId`, and `SimulationTime` remain the
authoritative deterministic synchronization values. A consumer must not infer
the CARLA frame solely from the formatted VISS timestamp. Vehicle-state values
are updated every simulation frame (nominally 20 Hz), while GNSS values update
at 10 Hz and retain their most recent individual data-point timestamp and
source-frame metadata. A fix is included only when its source frame is not
newer than the assembled vehicle frame and its simulated age is at most 0.25
seconds.

## Missing and unavailable data

The runtime does not replace unavailable optional values with zero. Invalid or
unavailable RPM and equivalent steering are omitted from the frame snapshot.
Missing, future, malformed, out-of-order, or stale GNSS fixes are likewise
omitted rather than replaced by zero coordinates. The GNSS callback handoff
retains at most one ordered fix and exposes accepted/rejected counters.
Read of an absent path returns VISS `404 unavailable_data`; optional points
appear automatically in later snapshots when their source is available.

## Frame store invariant

The designated owner advances CARLA by exactly one synchronous tick and then
builds one snapshot from that world frame. Vehicle-state points share the
anchored timestamp and metadata of that state frame. Retained 10 Hz GNSS points
keep the GNSS sensor's own timestamp and expose their source frame/time through
the simulation overlay. The snapshot store accepts only strictly increasing
state frame IDs and replaces its single retained snapshot atomically;
duplicates and out-of-order state frames are rejected. It never queues
historical snapshots.

## Not included in v0.1

Images, point clouds, radar detections, ultrasonic ranges, lane/collision
events, writable vehicle-control signals, a ROS 2 schema, and a non-VISS custom
network envelope are intentionally deferred.
