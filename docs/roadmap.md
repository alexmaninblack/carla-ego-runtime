# Roadmap

The project advances through small, independently verifiable milestones. No
camera, LiDAR, radar, or ultrasonic work is planned before the basic VISS
telemetry path is stable.

## M0 — Public project foundation

- [x] Create a standalone C++ repository.
- [x] Record architecture and public-repository boundaries.
- [x] Fix VISS 3.1 as the external service contract and VSS 6.0 as the signal
  model.
- [x] Document the initial VSS mapping, simulation overlay, and the optional
  role of ROS 2.
- [x] Provide a dependency-free buildable executable and smoke test.

Exit criterion: a clean checkout builds and tests without CARLA or Unreal
Engine source code, and the planned external interface is unambiguous.

## M1 — Native CARLA connection

- [x] Locate an installed LibCarla package through CMake.
- [x] Connect to a configurable host and RPC port.
- [x] Verify client/server version compatibility.
- [x] Select an existing `hero` vehicle or spawn the configured blueprint.
- [x] Destroy only actors owned by this runtime during shutdown.

Exit criterion: the runtime repeatedly connects, identifies one ego vehicle,
and exits cleanly on the Apple Silicon development machine.

## M2 — Basic vehicle state and VSS mapping

- [x] Enable synchronous mode with one designated tick owner.
- [x] Read velocity, acceleration, throttle, brake, steering command, gear, engine
  RPM, and front-wheel angles.
- [x] Maintain a transport-independent normalized internal state.
- [x] Convert values to the documented VSS paths, units, ranges, and ISO 8855 axes.
- [x] Generate and validate the versioned `Vehicle.CarlaSimulation.*` VSS overlay.
- [x] Add unit tests for projections, steering conversion, units, rounding, and
  validation ranges.

Exit criterion: the VSS signal store receives one correctly timestamped state
update for every simulation frame without an unbounded queue.

## M3 — GNSS

- [x] Attach one configurable `sensor.other.gnss` actor to the ego vehicle.
- [x] Receive fixes at 10 Hz and preserve CARLA frame/timestamp metadata.
- [x] Map fixes to the standard `Vehicle.CurrentLocation.*` paths.
- [x] Add lifecycle, stale-value, and missing-data handling.

Exit criterion: vehicle state and GNSS remain ordered and attributable to the
same simulator run during a continuous drive.

## M4 — VISS 3.1 service

- [x] Compare an embedded C++ endpoint with integration through COVESA VISSR.
- [x] Record the implementation decision and MPL-2.0 obligations.
- [x] Implement the documented JSON-over-Secure-WebSocket profile.
- [x] Support `get`, `subscribe`, `unsubscribe`, and protocol-valid read-only
  responses to `set`.
- [x] Add bounded buffering, reconnect handling, and dropped-update metrics.
- [x] Test TLS, malformed requests, timestamps, path selection, and subscriptions.
- [x] Test an independent consumer on the same Mac.
- [ ] Repeat the consumer test from a second networked machine after installing
  a trusted certificate and completing the deployment threat review.

Local exit criterion: satisfied. An independent VISS client receives the
documented live vehicle state and GNSS signals with observable latency and loss
behaviour, and the compatibility suite passes. External network exposure
remains deliberately gated by the security checklist.

## M5 — Driving and operational reliability

- [x] Add a BehaviorAgent route for the initial autonomous drive.
- [x] Keep the control source replaceable for future external control.
- [x] Add configuration validation, structured logging, and run manifests.
- [x] Maintain the pinned CARLA commit and reproducible installation runbook.
- [x] Run bounded endurance and restart tests.

Exit criterion: the ego vehicle drives a repeatable route while VISS telemetry
is streamed continuously and the runtime can recover from clean restarts.

Status: locally satisfied on the pinned Apple Silicon baseline. The acceptance
route runs at a measured 30 Hz cadence; independent VISS probes, resource
cleanup, and a two-run restart sequence are recorded in per-run manifests
outside the repository.

## M5.1 — Operator experience and health visibility

- [x] Provide one launcher for the simulator, routed drive, telemetry runtime,
  and VSS dashboard.
- [x] Show explicit startup stages while Unreal and CARLA are becoming ready.
- [x] Stop only launcher-owned processes and restore CARLA world state after a
  normal exit, failure, or operator interrupt.
- [x] Display VISS connection state, measured simulation cadence, dashboard
  delivery cadence, and local VISS event latency.
- [x] Validate the integrated launcher visually and through restart and
  30-minute endurance acceptance runs.

Exit criterion: one operator action reaches a visibly healthy driving dashboard
and one stop action cleans up the complete launcher-owned session. Health
indicators remain stable during the bounded endurance run, with no VISS loss or
owned CARLA actors left behind.

Status: satisfied on the pinned Apple Silicon baseline. The integrated desktop
entry point, operator interrupt, two-run restart, and 32-minute endurance
sequence all completed with clean resource restoration.

## M6 — External vehicle control

- [x] Define a versioned control contract for throttle, brake, and steering.
- [x] Preserve BehaviorAgent as a separately selectable control source.
- [x] Add controller ownership, command sequence, and heartbeat semantics.
- [x] Apply a safe stop when commands expire or the external controller
  disconnects.
- [x] Demonstrate an independent client taking and releasing vehicle control
  while VISS telemetry remains continuous.

Exit criterion: an authenticated local client can explicitly acquire control,
drive the ego vehicle, and release it; stale or lost commands always produce a
bounded safe stop without breaking telemetry or CARLA cleanup.

Status: satisfied on the pinned Apple Silicon baseline. The acceptance client
drove the vehicle, triggered and recovered from the 250 ms command deadline,
released ownership, and then verified disconnect safe stop with continuous
30 Hz telemetry. A bounded visual run, two clean restarts, motion measurement,
and actor/socket cleanup are recorded in private per-run artifacts.

## M6.1 — Keyboard driving and observable startup

- [x] Add a focused-window native macOS keyboard client over the M6 contract.
- [x] Map Up to throttle, Down to brake, and Left/Right to steering with bounded
  command ramps and brake priority.
- [x] Select safe stop on focus loss, Space, window close, client loss, or
  command timeout; require Enter to arm or resume.
- [x] Keep a live VSS dashboard visible in the launch Terminal.
- [x] Add a separate cold/warm launcher and machine-readable startup timeline.
- [x] Complete the operator-observed cold-start drive and cleanup acceptance.

Exit criterion: one desktop action cold-starts CARLA, shows measured startup
stages, opens a live telemetry dashboard and keyboard-control panel, permits a
manual drive, and cleans up all owned resources after the operator exits.

Status: satisfied on the pinned Apple Silicon baseline. The operator completed
the desktop cold start, manual keyboard drive, live-dashboard review, safe-stop
checks, and clean exit. The final private run manifest reports completion; the
controller is stopped with no owner, and its socket and token are removed.

## M6.2 — Live manual and automatic handover

- [x] Add explicit safe-stop, manual, and autopilot states to control contract
  version 2 while retaining version 1 compatibility.
- [x] Keep one ego actor and one synchronous tick owner through every handover.
- [x] Use deterministic synchronous Traffic Manager control for automatic mode.
- [x] Gate autopilot activation to a nearby, aligned driving lane and blend the
  automatic-to-manual control transition.
- [x] Extend the native panel with direct Manual Control, Autopilot, and Safe
  Stop actions while keeping VSS telemetry visible.
- [ ] Complete the clean-start operator-observed handover and cleanup acceptance.

Exit criterion: one desktop session can switch repeatedly between manual and
automatic driving without changing the actor or interrupting VISS, and every
loss or exit path selects safe stop and cleans up owned resources.

Status: implemented and under acceptance on the pinned Apple Silicon baseline.

## Deferred

Cameras, LiDAR, radar, ultrasonic modelling, and packaged macOS distribution
are separate future milestones. Previously discussed M7 and M8 work is not
scheduled and remains explicitly postponed. ROS 2 may be added as an optional
adapter only when a concrete ROS-based consumer or tool is in scope; it is not
part of the core CARLA-to-VISS path.
