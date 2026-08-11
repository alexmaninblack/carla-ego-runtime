# Role of ROS 2

## Decision

ROS 2 is not required in the core CARLA-to-VISS telemetry path and is not an
initial runtime dependency.

The core path is:

```text
CARLA -> collectors -> normalized state -> VSS signal store -> VISS server
```

If a future consumer requires ROS 2, an optional adapter can branch from the
normalized state or VSS store. The VSS signal model remains the canonical
external data model, and VISS remains the required network contract.

## What ROS 2 would provide

ROS 2 is useful when the system is being assembled from multiple robotics
components that already use its conventions. It provides:

- discovery and publish/subscribe communication through DDS;
- QoS policies for reliability, durability, history, and deadlines;
- common messages used by autonomous-driving and robotics software;
- coordinate-frame distribution through TF;
- simulated time through `/clock`;
- recording and replay through rosbag;
- visualization and inspection through tools such as RViz;
- direct integration with ROS-based stacks such as Autoware.

These are valuable ecosystem and orchestration capabilities. They are not
needed merely to read CARLA state, map it to VSS paths, and serve VISS clients.

## Why it is not in the initial path

Putting ROS 2 between CARLA and VISS would add another middleware runtime,
schema mapping, discovery mechanism, QoS configuration, build toolchain, and
failure boundary. It would also force every deployment to carry ROS 2 even if
the only consumer speaks VISS.

For the first basic signals, the proposed pipeline already has one producer,
one canonical signal model, and one required network API. ROS 2 would not add
missing semantics; it would duplicate transport and data-model work.

## When to add the adapter

Add a ROS 2 adapter when at least one concrete requirement exists:

- an Autoware or other ROS 2 driving stack must consume the simulated vehicle;
- RViz, rosbag, TF, or standard ROS sensor messages are part of the test plan;
- multiple independent ROS nodes require DDS discovery and QoS;
- bidirectional ROS control is explicitly designed and safety-bounded.

At that point the adapter should have its own milestone and tests for time,
coordinate frames, QoS, backpressure, and mapping between VSS paths and ROS
messages. It must remain optional so a native macOS VISS deployment does not
depend on ROS 2.
