# ADR 0002: Basic telemetry and GNSS first

- Status: Accepted
- Date: 2026-08-11

## Context

High-bandwidth perception sensors add rendering, synchronization, compression,
and network-performance concerns before the vehicle lifecycle and message
semantics have been validated.

## Decision

The first milestone includes only speed, physical acceleration, throttle,
brake, normalized steering command, actual front-wheel steering angles, gear,
engine RPM, simulation timing metadata, and GNSS position.

Cameras, LiDAR, radar, ultrasonic modelling, and ROS 2 integration are deferred.

## Consequences

The initial data path is inexpensive and testable. The contract distinguishes
control inputs from physical vehicle state, so future sensors can be added
without redefining the original fields.
