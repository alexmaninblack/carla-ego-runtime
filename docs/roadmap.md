# Roadmap

The project advances through small, independently verifiable milestones. No
camera, LiDAR, radar, or ultrasonic work is planned before the basic telemetry
path is stable.

## M0 — Public project foundation

- [x] Create a standalone C++ repository.
- [x] Record architecture and public-repository boundaries.
- [x] Draft the semantic telemetry contract.
- [x] Provide a dependency-free buildable executable and smoke test.

Exit criterion: a clean checkout builds and tests without CARLA or Unreal
Engine source code.

## M1 — Native CARLA connection

- Locate an installed LibCarla package through CMake.
- Connect to a configurable host and RPC port.
- Verify client/server version compatibility.
- Select an existing `hero` vehicle or spawn the configured blueprint.
- Destroy only actors owned by this runtime during shutdown.

Exit criterion: the runtime repeatedly connects, identifies one ego vehicle,
and exits cleanly on the Apple Silicon development machine.

## M2 — Basic vehicle state

- Enable synchronous mode with one designated tick owner.
- Read speed, world acceleration, throttle, brake, steering command, gear,
  engine RPM, and front-wheel angles.
- Convert and label all units explicitly.
- Emit a human-readable diagnostic representation of `VehicleState v0`.
- Add unit tests for projections, units, and validation ranges.

Exit criterion: one correctly timestamped state record is produced for every
simulation frame without an unbounded queue.

## M3 — GNSS

- Attach one configurable `sensor.other.gnss` actor to the ego vehicle.
- Receive fixes at 10 Hz and preserve CARLA frame/timestamp metadata.
- Add lifecycle and missing-data handling.

Exit criterion: vehicle state and GNSS remain ordered and attributable to the
same simulator run during a continuous drive.

## M4 — External transport

Decision gate: identify the first consumer and choose the transport only then
(for example gRPC/Protobuf, ROS 2, or another protocol).

- Define a versioned wire schema from the semantic v0 contract.
- Implement the publisher adapter and heartbeat.
- Add bounded buffering, disconnect recovery, and dropped-message metrics.
- Test a consumer on the same Mac and on a second networked machine.

Exit criterion: an external consumer receives documented state and GNSS data
with observable latency and loss behaviour.

## M5 — Driving and operational reliability

- Add a BehaviorAgent route for the initial autonomous drive.
- Keep the control source replaceable for future external control.
- Add configuration validation, structured logging, and run manifests.
- Pin the tested CARLA commit and document reproducible installation.
- Run endurance and restart tests.

Exit criterion: the ego vehicle drives a repeatable route while basic telemetry
is streamed continuously and the runtime can recover from clean restarts.

## Deferred

Cameras, LiDAR, radar, ultrasonic modelling, native ROS 2 inside the CARLA
server, packaged macOS distribution, and external autonomous control are
separate future milestones.
