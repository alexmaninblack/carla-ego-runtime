# VISS/VSS telemetry contract v0.4

Status: **implemented for the vehicle-state, GNSS, VISS network, offline
strict-role mTLS/assignment boundary, frame-coherent Safe Stop projection and
live controller-to-Gateway handoff subsets**.

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
| `Vehicle.Chassis.Axle.Row{1,2}.Wheel.{Left,Right}.AngularSpeed` | `float`, degrees/s | Magnitude of the live Unreal Chaos wheel angular velocity, converted from the normalized rad/s sample to degrees/s (`x 180/pi`) only at the VSS projection boundary. CARLA wheel order is validated as front-left, front-right, rear-left, rear-right. |
| `Vehicle.Chassis.Axle.Row{1,2}.Wheel.{Left,Right}.Speed` | `float`, km/h | Non-negative linear wheel speed derived from live angular speed and that wheel's configured physical radius. |
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
| `Vehicle.CarlaSimulation.ProfileVersion` | `string` attribute | Version of this compatibility profile, currently `0.2`. |
| `Vehicle.CarlaSimulation.RunId` | `string` sensor | Unique identifier of one simulator run. |
| `Vehicle.CarlaSimulation.EgoVehicleId` | `string` sensor | Stable ego-vehicle identifier within the run. |
| `Vehicle.CarlaSimulation.FrameId` | `uint64` sensor | CARLA simulation frame represented by the sample. |
| `Vehicle.CarlaSimulation.SimulationTime` | `double`, s | Exact CARLA elapsed simulation time. |
| `Vehicle.CarlaSimulation.GnssFrameId` | `uint64` sensor | Source CARLA frame of the retained GNSS fix. |
| `Vehicle.CarlaSimulation.GnssSimulationTime` | `double`, s | Source CARLA simulation time of the retained GNSS fix. |
| `Vehicle.CarlaSimulation.Control.ActiveMode` | `string` sensor | Applied `SAFE_STOP`, `SCENARIO`, `MANUAL` or `AUTOPILOT` fact for the same CARLA frame. |
| `Vehicle.CarlaSimulation.Control.TransitionState` | `string` sensor | Applied `STABLE`, `PREPARING` or `FAILED` transition fact for the same CARLA frame. |
| `Vehicle.CarlaSimulation.Control.Generation` | `uint64` sensor | Accepted control transaction generation attributable to the same frame. |
| `Vehicle.CarlaSimulation.Reset.Generation` | `uint64` sensor | Generation of the latest successful canonical reset attributable to the same frame. |
| `Vehicle.CarlaSimulation.Reset.InProgress` | `boolean` sensor | Whether canonical physical reset work is still in progress for the same frame. |
| `Vehicle.CarlaSimulation.Reset.Discontinuity` | `boolean` sensor | Whether the current complete frame is the first post-reset discontinuity frame. |
| `Vehicle.CarlaSimulation.ChaosWheel.Row{1,2}.{Left,Right}.LateralSlipAngle` | `double`, degrees | Signed lateral slip angle reported by the live Chaos wheel state. This is simulator-specific and is not claimed to be a standard VSS signal. |
| `Vehicle.CarlaSimulation.ChaosWheel.Row{1,2}.{Left,Right}.LongitudinalSlip` | `double` | Longitudinal slip magnitude reported by the live Chaos wheel state. This is simulator-specific and is not claimed to be a standard VSS signal. |

The versioned artifact is
[`vss/Vehicle.CarlaSimulation.vspec`](../vss/Vehicle.CarlaSimulation.vspec).
It is structurally validated in the dependency-free test suite and was also
merged with the complete VSS 6.0 catalog under `vss-tools` 6.0 `--strict`.
The merged tree confirms the documented types and units. The overlay uses a
project namespace and is not presented as part of standard COVESA VSS.

The six control/reset points form one optional, all-or-none fact group. The
live C++ channel accepts one closed controller record only when its run ID and
ego actor match the selected observer context and its frame ID plus simulation
time exactly match the physical vehicle state. Missing, truncated, malformed,
wrong-identity, duplicate, out-of-order, expired or generation-regressing
context therefore keeps all six unavailable while truthful physical telemetry
remains available. The projector never copies an older context forward,
invents a default, evaluates Safe Stop thresholds or authorizes a Platform
update. All six points use the physical frame's existing UTC source timestamp;
VISS encodes boolean datapoint values as the strings `true` and `false`, not
numeric substitutes.

The live facts handoff is one private, connected, non-blocking Unix stream
created for one run. Its owner-only listener accepts exactly one connection;
both endpoints verify the peer effective UID through Darwin `getpeereid` or
Linux `SO_PEERCRED`. Each body has an unsigned big-endian 32-bit length, is at
most 4096 bytes, and the receiver retains at most one bounded partial frame.
Zero/oversize/truncated input, backpressure, partial write, EOF or disconnect
makes the channel unavailable. The receiver holds at most four unmatched
physical and four unmatched controller records for at most 250 ms and keeps
saturating diagnostic counters only. It does not use
`controller-status.json`, reuse last-known data, retain history or implement
reconnect.
The Gateway now implements the purpose-bound `PLATFORM_UPDATE_RUNTIME` mTLS
role and its exact ten-path read allowlist:
`Vehicle.Speed`, accelerator and brake pedal position,
`Vehicle.CarlaSimulation.FrameId`, the three Control active/transition/
generation facts, and the three Reset generation/in-progress/discontinuity
facts. The selected VDP transport, Engineering Dashboard, and qualification
clients have separate compiled read-only policies. Assignment generation and
the post-selection exclusive frame floor prevent a newly selected Unit from
receiving the previously retained snapshot. This is an offline-validated
Gateway boundary only; onboarding delivery of per-Unit credentials and live
VM/Unit qualification remain separate work.

## Time and synchronization

VISS timestamps use ISO 8601 UTC with a trailing `Z`. They represent real UTC
at **Gateway acquisition**, not an extrapolation from CARLA simulation time.
The Gateway captures `system_clock::now()` once immediately after receiving a
world snapshot, before telemetry RPCs, camera work or control-fact matching.
Every physical point and matched control/reset fact in that frame carries this
same timestamp. A GNSS callback captures its own acquisition UTC once; later
normalization and merging preserve it. Reads, subscriptions and retained
snapshots never refresh timestamps. Duplicate/out-of-order frame rejection is
unchanged. A slow collector or stopped stream therefore becomes stale rather
than being made fresh by a later VISS request.

This operator-approved amendment (2026-09-06) supersedes the original
simulation-time-to-UTC anchor. Pauses, reset work and faster/slower simulation
must not accumulate an offset against the VM's real-time freshness check.
Simulation time remains separate and deterministic; UTC need not advance at
simulation speed or be monotonic across a host clock correction. Downstream
freshness/future-time checks remain unchanged. This timestamp describes
Gateway acquisition, not independently measured simulator-side capture or
network transit time; it is qualified here for the local CARLA/Gateway setup.

`Vehicle.CarlaSimulation.RunId`, `FrameId`, and `SimulationTime` remain the
authoritative deterministic synchronization values. A consumer must not infer
the CARLA frame solely from the formatted VISS timestamp. Vehicle-state values
are updated every simulation frame (30 Hz for the M5 route; the standalone
runtime default remains 20 Hz), while GNSS values update at 10 Hz and retain
their most recent individual data-point timestamp and
source-frame metadata. A fix is included only when its source frame is not
newer than the assembled vehicle frame and its simulated age is at most 0.25
seconds.

## Missing and unavailable data

The runtime does not replace unavailable optional values with zero. Invalid or
unavailable RPM and equivalent steering are omitted from the frame snapshot.
Missing, future, malformed, out-of-order, or stale GNSS fixes are likewise
omitted rather than replaced by zero coordinates. The GNSS callback handoff
retains at most one ordered fix and exposes accepted/rejected counters.
Missing, non-finite, or structurally incomplete wheel telemetry is also
omitted per wheel. A missing wheel never becomes a plausible zero-speed or
zero-slip sample.
Read of an absent path returns VISS `404 unavailable_data`; optional points
appear automatically in later snapshots when their source is available.

## Frame store invariant

The designated owner advances CARLA by exactly one synchronous tick and then
builds one snapshot from that world frame. Vehicle-state points share the
acquisition timestamp and metadata of that state frame. Retained 10 Hz GNSS points
keep the GNSS sensor's own timestamp and expose their source frame/time through
the simulation overlay. The snapshot store accepts only strictly increasing
state frame IDs and replaces its single retained snapshot atomically;
duplicates and out-of-order state frames are rejected. It never queues
historical snapshots.

## Not included in v0.1

Images, point clouds, radar detections, ultrasonic ranges, lane/collision
events, writable vehicle-control signals, a ROS 2 schema, and a non-VISS custom
network envelope are intentionally deferred.
