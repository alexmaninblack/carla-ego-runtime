# ADR 0006 — Frame-aligned latest-value store

Status: accepted

## Context

Vehicle state must remain attributable to one CARLA simulation frame. A queue
between the collector and the future VISS server could grow without bound when
a consumer is slower than the simulator, and independent RPC reads without a
tick boundary could mix adjacent frames.

## Decision

The runtime is the single designated synchronous tick owner by default. It
sets a 0.05 second fixed delta, advances one frame, reads the matching actor
snapshot and vehicle telemetry, and publishes one VSS snapshot. Vehicle-state
points share the state frame's anchored UTC timestamp, frame ID, and simulation
time. Slower sensors such as the 10 Hz GNSS collector retain their own source
timestamp and expose source frame/time metadata when merged into a later state
snapshot.

The in-process store retains only the newest complete snapshot. It accepts only
strictly increasing frame IDs and rejects duplicates or older frames. Observer
mode is explicit and never applies settings or advances the world.

## Consequences

Memory use is bounded independently of runtime duration and a future VISS
consumer cannot create collector backpressure by leaving historical snapshots
unread. Consumers that require history must persist it outside this store. Only
one process may be the tick owner; multi-client deployments must designate it
operationally.
