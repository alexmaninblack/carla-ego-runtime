# ADR 0008 — Replaceable external control source

Status: accepted

## Context

M5 needs a repeatable autonomous route without coupling telemetry collection to
one driving implementation. CARLA's maintained BehaviorAgent is Python code,
while the VISS runtime and native sensor collectors are C++.

## Decision

The Python control source is the single synchronous tick owner. It creates and
owns the ego vehicle, computes a configured route with CARLA BehaviorAgent, and
applies vehicle controls. The C++ runtime runs with `--no-spawn
--observe-ticks`; it owns only its GNSS actor, VSS state, VISS endpoint, and
optional spectator camera.

The boundary is process-based and identified in every run manifest as
`control_source`. A future controller can replace BehaviorAgent while keeping
the runtime command and VISS contract unchanged, provided it owns the same
vehicle role and simulation clock.

Startup and shutdown use file gates. During both gates the controller continues
advancing the synchronous world, so a newly connected observer receives actor
state and can clean up its sensor before the vehicle is destroyed.

## Consequences

- Control and telemetry can fail, restart, and be tested independently.
- The Python wheel and BehaviorAgent must come from the pinned CARLA commit.
- Only the controller destroys the vehicle; only the runtime destroys its GNSS
  sensor.
- The external VISS interface remains independent of Python and ROS 2.
