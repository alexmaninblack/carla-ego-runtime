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
| Default development port | `6443`, configurable; loopback only |
| Transport security | TLS 1.2 or newer |
| Strict client identity | CA-verified X.509 leaf with one role URI SAN |
| Primary payload | VISS JSON |
| Client operations | `get`, `subscribe`, `unsubscribe` |
| Update operation | Sensors remain read-only; only the two D4-008 typed QM advisory targets accept authenticated selected-VDP Set |
| Signal model | VSS 6.0 plus `Vehicle.CarlaSimulation.*` overlay |
| Nominal state cadence | 30 Hz for M5; 20 Hz standalone default |
| Nominal GNSS cadence | 10 Hz |

Secure WebSocket is selected because it supports both point reads and
server-pushed subscriptions over one connection. HTTP, MQTT, gRPC, Unix-domain
sockets, Protocol Buffers, and VISS data compression are outside the first
VISS client-data transport profile, not prohibited forever. The private
assignment-control socket described below is a separate local control plane.

## Operations

- **Get** reads one path or a supported multi-path selection from the current
  signal store.
- **Subscribe** creates a server-pushed telemetry stream and returns a
  `subscriptionId`.
- **Unsubscribe** uses the same WebSocket connection and subscription
  identifier that created the subscription.
- **Set** rejects sensors, arbitrary paths and all motion/control writes.
  The selected platform role may write only the two D4-008 typed QM Request
  leaves described below; independent/dashboard and update-runtime roles
  cannot write them. No request body can choose an identity or authority.

## Typed QM advisory boundary (D4-008)

The Gateway implements the accepted solution `qm-advisory-profile` 1.1.0:
`Vehicle.OEM.BrakeHealth.Advisory.Request` and
`Vehicle.OEM.TireHealth.Advisory.Request`, with the corresponding
`GatewayStatus` sensors. The VISS Set value is the unchanged canonical JSON
string, never an object-shaped VISS value or arbitrary display text. Functional
compatibility remains Brake v3 / Tire v1; `serviceVersion` is actual numeric
package-release provenance, not authorization or a hard-coded release list.

The current assignment authenticates the selected VDP certificate. Gateway
revalidates exact schema/keys, endpoint enums, 2048-byte request limit, UUID and
sequence identity, UTC dates, at-most-two-second acceptance age and 30-second
lease. Refresh is bounded to 10 seconds; changes to one second. A bounded
512-entry replay cache per endpoint retains accepted identities for five
minutes and is shared across WebSocket reconnects. Different content for the
same identity and sequence rollback fail closed. Identical repeats do not
renew leases or replace newer state. Neither Set success nor service intent
is presented as application evidence: GatewayStatus is the authority.

Request/status leaves exist as empty unavailable values before the first
accepted request. They may be included in the existing telemetry subscription
or read by Get; no second selected-role connection is required or enabled.
Lease expiry uses both UTC and a monotonic deadline. It publishes EXPIRED and
clears active fields even if the producer disappears. Assignment handover
clears both advisory indications. The native dashboard reads only these
Gateway values and never contacts the VM, Cloud or functional backend.

Restart qualification remains explicitly incomplete: no new durable replay
store has been introduced. Envelopes issued before the Gateway process
started are rejected and cannot reapply a retained target. Accepted valid
duplicate reconciliation across a whole Gateway process restart still needs
an approved persistence/reconstruction decision. Reconnect within the same
Gateway process is implemented and tested. This host-tested source change is
not evidence of deployed or live end-to-end advisory operation.

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
- The explicit development profile performs server authentication only and is
  restricted to `127.0.0.1` or `::1`. It is not strict-mode evidence.
- Strict mode requires a client CA bundle, restored assignment generation,
  private assignment-socket path, Dashboard leaf fingerprint, and an optional
  qualification leaf fingerprint. It never falls back to development mode.
- A strict client leaf must be currently valid, chain to the configured client
  CA, contain `digitalSignature` key usage and `clientAuth` EKU, and contain
  exactly one recognized URI SAN. Its enrollment fingerprint is lowercase
  SHA-256 over the DER leaf bytes.
- Selected roles use
  `urn:aosedge:demo:viss-client:v1:<role>:<unit-uuid>:<node-uuid>`, where the
  role is `selected-platform-unit` or `platform-update-runtime`. Independent
  roles use the exact `engineering-dashboard` or `qualification-client` URI.
  CN, OU, source IP, DNS name, and request payload are not identity authority.
- The bundled client accepts explicit `--cert` and `--key` inputs. It consumes
  credentials supplied by its caller and does not create or retain them.

### Assignment and role policy

Strict mode starts detached at the generation restored by the caller. One
owner-only `AF_UNIX/SOCK_STREAM` control socket accepts same-effective-UID
`select`, `detach`, and `status` requests. The socket parent is mode `0700`, the
socket is mode `0600`, and each connection carries one newline-terminated JSON
request of at most 4096 bytes. Selection uses an exact generation compare-and-
swap, canonical Unit/Main Node UUIDs, and distinct fingerprints for both
selected roles. Replacement requires an explicit detach.

Strict mode admits at most one live session for each of four roles and at most
four sessions globally. The selected Platform Unit follows the compiled
D4-006 selected-transport read policy. Platform Update Runtime can read only
the ten frozen Safe Stop paths. Dashboard and qualification clients use the
independent read-only policy. Every Set is denied, and an unauthorized read or
subscription returns ordinary unavailable data without disclosing another
role's path set.

On selection, the latest complete frame becomes an exclusive lower bound for
both selected roles. They receive no cached data until a newer complete frame
is published. Detach or another assignment-generation change closes both
selected sessions and clears their subscription state; independent Dashboard
delivery remains live. Certificate time validity is checked at handshake.
There is no first-demo CRL/OCSP fetch, rotation daemon, or mid-session expiry
timer.

## Implementation and conformance

The runtime embeds a Boost.Beast/Asio and OpenSSL endpoint. The comparison with
[COVESA VISSR](https://github.com/COVESA/vissr), licence analysis, version pins,
and rationale are recorded in
[ADR 0007](decisions/0007-embedded-viss-endpoint.md). No COVESA implementation
source or schema is copied into this repository.

Protocol tests cover path reads, string values, multi-path selection,
timestamps, subscription lifecycle, reconnect isolation, read-only Update
errors, malformed requests, role filtering, generation frame floors, and
limits. A separate client/server test creates an ephemeral CA and all leaf
credentials at runtime. It verifies real loopback mTLS rejection and role
admission, the `VISSv3` handshake, the four-role cap, detach/reselect session
closure, independent Dashboard continuity, and pre-assignment frame isolation.
The temporary credentials are destroyed when the test ends.

The assignment transport has native same-UID and owner/mode/owned-inode
coverage on macOS. A wrong-UID native macOS execution is not claimed because
the fixture would require a privileged second user. The Linux implementation
uses `SO_PEERCRED`; Darwin uses `getpeereid`.

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
