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
route, independent VISS probes, resource cleanup, and two-run restart sequence
are recorded in per-run manifests outside the repository.

## Deferred

Cameras, LiDAR, radar, ultrasonic modelling, packaged macOS distribution, and
external autonomous control are separate future milestones. ROS 2 may be added
as an optional adapter only when a concrete ROS-based consumer or tool is in
scope; it is not part of the core CARLA-to-VISS path.
