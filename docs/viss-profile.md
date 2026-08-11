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
| Nominal state cadence | 20 Hz |
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

The first subscription implementation may support only the filter forms
required by the initial consumer. Unsupported standard options must receive a
protocol-valid error rather than being silently ignored.

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
- Exposing the service to another machine requires an explicit bind address,
  trusted TLS material, and a documented threat review.

## Implementation and conformance

M4 will compare two implementation strategies:

1. embed a C++ VISS endpoint in this runtime; or
2. connect the runtime's VSS signal store to
   [COVESA VISSR](https://github.com/COVESA/vissr).

The choice must be based on macOS/Apple Silicon support, licence obligations,
conformance behaviour, operational complexity, and measured latency. The
public VISS/VSS contract remains the same in either case.

The initial server is complete only when a separate client test verifies path
reads, subscriptions, timestamps, units, reconnect behaviour, read-only Update
errors, TLS, and rejection of malformed requests.
