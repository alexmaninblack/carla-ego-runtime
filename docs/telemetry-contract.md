# VISS/VSS telemetry contract v0.1

Status: **accepted planning contract**. Implementation and conformance testing
remain pending.

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

The precise steering conversion, coordinate conversion, rounding, and missing
value policy must be unit-tested before the contract is marked implemented.

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

## CARLA simulation overlay

The following project-owned paths extend, but do not modify, VSS 6.0:

| Extension path | Type and unit | Meaning |
| --- | --- | --- |
| `Vehicle.CarlaSimulation.ProfileVersion` | `string` attribute | Version of this compatibility profile, initially `0.1`. |
| `Vehicle.CarlaSimulation.RunId` | `string` sensor | Unique identifier of one simulator run. |
| `Vehicle.CarlaSimulation.EgoVehicleId` | `string` sensor | Stable ego-vehicle identifier within the run. |
| `Vehicle.CarlaSimulation.FrameId` | `uint64` sensor | CARLA simulation frame represented by the sample. |
| `Vehicle.CarlaSimulation.SimulationTime` | `double`, s | Exact CARLA elapsed simulation time. |

The overlay will be maintained as a versioned `.vspec` artifact before the
VISS server is implemented. It must use a project namespace and must not be
presented as part of standard COVESA VSS.

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
at 10 Hz and retain their most recent individual data-point timestamp.

## Missing and unavailable data

The runtime must not replace unavailable values with zero. It either omits the
data point from a multi-path response or returns the VISS error appropriate to
the requested path/state. The exact policy will be validated against the
chosen server implementation.

## Not included in v0.1

Images, point clouds, radar detections, ultrasonic ranges, lane/collision
events, writable vehicle-control signals, a ROS 2 schema, and a non-VISS custom
network envelope are intentionally deferred.
