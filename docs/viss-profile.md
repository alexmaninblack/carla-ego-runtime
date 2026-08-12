# VISS 3.1 compatibility profile

## Purpose

This document narrows the implementation choices allowed by COVESA VISS 3.1 so
that the first CARLA client and server can interoperate without private
assumptions. VISS defines the service; VSS 6.0 defines the signal tree.

## Pinned standards

- [COVESA VISS 3.1](https://github.com/COVESA/vehicle-information-service-specification/tree/v3.1)
- [COVESA VSS 6.0](https://github.com/COVESA/vehicle_signal_specification/tree/v6.0)

The discontinued W3C VISS 2 draft is useful history but is not the target for
this project. Upgrading either pinned version requires a reviewed contract
change and compatibility notes.

## Initial wire profile

| Concern | Initial choice |
| --- | --- |
| Transport | Secure WebSocket (`wss`) |
| WebSocket subprotocol | `VISSv3` |
| Default development port | `6443`, configurable |
| Transport security | TLS 1.2 or newer |
| Primary payload | VISS JSON |
| Client operations | `get`, `subscribe`, `unsubscribe` |
| Update operation | Request syntax and standard errors supported; all initial signal nodes are read-only |
| Signal model | VSS 6.0 plus `Vehicle.CarlaSimulation.*` overlay |
| Nominal state cadence | 30 Hz for M5; 20 Hz standalone default |
| Nominal GNSS cadence | 10 Hz |

Secure WebSocket is selected because it supports both point reads and
server-pushed subscriptions over one connection. HTTP, MQTT, gRPC, Unix-domain
sockets, Protocol Buffers, and VISS data compression are outside the first
interoperability profile, not prohibited forever.

## Operations

- **Get** reads one path or a supported multi-path selection from the current
  signal store.
- **Subscribe** creates a server-pushed telemetry stream and returns a
  `subscriptionId`.
- **Unsubscribe** uses the same WebSocket connection and subscription
  identifier that created the subscription.
- **Set** is parsed because VISS transports are required to support Update.
  Since v0.1 exposes sensors and attributes only, attempts to update them
  return the appropriate standard VISS error and never control the vehicle.

The implemented filter subset is a `paths` filter for Read and a required
`timebased` filter plus optional `paths` filter for Subscribe. Time-based
periods are decimal millisecond strings from `50` through `60000`. Relative
path patterns may contain `*`. Unsupported standard options receive a
protocol-valid error rather than being silently ignored.

VISS values are encoded as strings, including numeric data. A Read of one leaf
returns one data object; a branch or multi-path selection returns an array.
Every data point preserves its source timestamp and every response/event has a
separate server-execution `ts`. Subscription state belongs to the WebSocket
connection that created it and is discarded on reconnect.

Example Read request:

```json
{"action":"get","path":"Vehicle.Speed","requestId":"speed-1"}
```

Example subscription request:

```json
{"action":"subscribe","path":"Vehicle","filter":[{"variant":"paths","parameter":["Speed","CurrentLocation.*"]},{"variant":"timebased","parameter":{"period":"100"}}],"requestId":"telemetry-1"}
```

## Signal tree and timestamps

Standard paths, units, transformations, simulation extensions, and timestamp
rules are normative for this project and are listed in the
[telemetry contract](telemetry-contract.md).

## Security and deployment

- The endpoint must not offer unencrypted `ws` outside an explicitly isolated
  local test harness.
- Development certificates, keys, and access tokens must never be committed.
- Binding to loopback is the default until authentication and authorization
  have been selected and tested.
- The M4 profile implements no authorization scheme. A request containing an
  `authorization` field receives an unsupported-feature error; TLS protects the
  transport but does not authenticate a client.
- Exposing the service to another machine requires an explicit bind address,
  trusted TLS material, and a documented threat review.

## Implementation and conformance

The runtime embeds a Boost.Beast/Asio and OpenSSL endpoint. The comparison with
[COVESA VISSR](https://github.com/COVESA/vissr), licence analysis, version pins,
and rationale are recorded in
[ADR 0007](decisions/0007-embedded-viss-endpoint.md). No COVESA implementation
source or schema is copied into this repository.

Protocol tests cover path reads, string values, multi-path selection,
timestamps, subscription lifecycle, reconnect isolation, read-only Update
errors, malformed requests, and limits. A separate client/server test creates
ephemeral TLS material at runtime and verifies TLS, the `VISSv3` handshake,
push events, GNSS reads, and metrics across the actual network boundary.

## Flow control and metrics

The VSS store retains one complete frame. Each client has a bounded outbound
queue and bounded subscription set. Protocol responses take priority over
queued subscription events; when the event queue is full, new events are
dropped instead of slowing the simulator. If multiple requested time periods
elapse before service, only the current snapshot is sent.

Shutdown output exposes accepted/rejected and active connection counts,
requests, protocol errors, emitted subscription events, dropped events, and
coalesced intervals. These counters describe server-side delivery. A consumer
that requires durable history or end-to-end acknowledgements must persist and
measure them outside this latest-value service.
