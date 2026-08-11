# ADR 0003: VISS is the external telemetry interface

- Status: Accepted
- Date: 2026-08-11

## Context

The earlier scaffold deferred its wire format and transport until an external
consumer was identified. The project now requires vehicle telemetry to be
available through the Vehicle Information Service Specification. Calling VISS
only a message format would be misleading: it defines operations, payloads,
transports, subscriptions, errors, and access to a hierarchical signal model.

The W3C VISS 2 draft is discontinued. COVESA continues the specification and
has released VISS 3.1. COVESA VSS 6.0 is this project's pinned signal model.

## Decision

Use COVESA VISS 3.1 as the external service contract and VSS 6.0 as the signal
tree. The initial interoperability profile is JSON over Secure WebSocket with
reads and subscriptions. The initial tree is read-only: Update request handling
must be protocol-valid, but no external vehicle-control nodes are exposed.

Standard VSS paths are used wherever their meaning matches. CARLA run, frame,
and simulation-time metadata is exposed through a versioned
`Vehicle.CarlaSimulation.*` overlay.

## Consequences

Transport and signal semantics are no longer open-ended. Internal collectors
remain independent of VISS, allowing mapping and unit conversion to be tested.
The implementation must validate conformance, TLS, subscriptions, errors, and
MPL-2.0 obligations. Choosing an embedded endpoint or COVESA VISSR is deferred
without reopening the public contract.
