# ADR 0004: ROS 2 is an optional adapter

- Status: Accepted
- Date: 2026-08-11

## Context

ROS 2 provides valuable robotics integration, DDS publish/subscribe and QoS,
TF, simulated time, rosbag, RViz, and compatibility with autonomous-driving
stacks. None of those capabilities is necessary merely to collect basic CARLA
state and serve it through the required VISS interface.

Making ROS 2 mandatory would introduce a second middleware and data model into
every deployment, including native macOS deployments whose clients only use
VISS.

## Decision

Do not place ROS 2 in the core runtime path and do not make it an initial build
or runtime dependency. If a concrete ROS-based consumer appears, implement a
separately configurable adapter from the normalized state or VSS signal store.
The VSS signal store remains canonical and VISS remains the required network
contract.

## Consequences

The initial architecture stays smaller and has one network API. Autoware, RViz,
rosbag, TF, and ROS message integration remain possible without forcing their
dependencies onto VISS-only users. A future adapter requires its own time,
coordinate-frame, QoS, backpressure, and control-safety tests.
