# ADR 0007 — Embedded VISS 3.1 endpoint

Status: accepted

## Context

The Apple Silicon runtime needs a VISS 3.1 service. The two candidate
architectures were an endpoint embedded in this C++ process and a feed from the
runtime into COVESA VISSR. The comparison was made against COVESA VISS 3.1 at
commit
[`0b0946885861939345d9a89797ed0913f9d4e218`](https://github.com/COVESA/vehicle-information-service-specification/commit/0b0946885861939345d9a89797ed0913f9d4e218)
and VISSR at commit
[`19130e0732d57c241512b59c2953c95160830c15`](https://github.com/COVESA/vissr/commit/19130e0732d57c241512b59c2953c95160830c15).

| Criterion | Embedded C++ endpoint | COVESA VISSR integration |
| --- | --- | --- |
| Apple Silicon build | Reuses the C++20 and Boost headers already installed with LibCarla plus Homebrew OpenSSL | Requires a separate Go toolchain and multiple VISSR processes/components |
| Frame and timestamp ownership | Reads the in-process immutable latest snapshot directly | Requires a feeder protocol and another storage handoff |
| Operational surface | One runtime process and one TLS listener | Server, service manager, storage, and feeder configuration |
| VISS breadth | Deliberately limited documented profile | Broader transports, filters, authorization, and service discovery |
| Conformance risk | Project must test every supported operation | More behavior is inherited from the reference implementation |
| Licence impact | New project code remains MIT; Boost is BSL-1.0 and OpenSSL is Apache-2.0 | Reused or modified VISSR source is MPL-2.0 and requires its notices/file-level obligations |

VISSR is a useful full reference stack and compatibility oracle, but its
additional storage and feeder boundary do not add value for the first single
vehicle, latest-value-only profile.

## Decision

Implement the endpoint in this process with Boost.Beast/Asio and
OpenSSL. It is a clean implementation from the public VISS 3.1 specification;
no VISSR or VISS specification source, schema, or generated artifact is copied
into the repository.

The listener is Secure WebSocket only, requires TLS 1.2 or newer, and accepts
only the `VISSv3` subprotocol. It binds to loopback by default. Subscription
state belongs to one connection and is discarded on disconnect. The signal
store keeps one frame, every client queue and subscription set has a configured
bound, and slow-consumer event drops and elapsed subscription intervals are
observable in shutdown metrics.

The implemented filter subset is:

- Read with an optional `paths` filter;
- Subscribe with one required `timebased` filter and an optional `paths`
  filter;
- periods from 50 through 60000 milliseconds;
- `*` wildcard matching inside relative path selections.

Unsupported VISS features receive a protocol error rather than being ignored.
Telemetry sensors and attributes remain read-only. General Update requests
are validated and rejected with VISS `400 invalid_data`; only the accepted
typed Brake/Tire advisory Request and Availability leaves permit Set by the
authenticated selected platform peer. Strict mTLS roles and exact allowlists
are defined by the [current VISS profile](../viss-profile.md). This narrow
advisory channel grants no vehicle-motion authority.

## Consequences

The runtime has no Go, VISSR, feeder, or external database dependency. The
small service profile is independently testable and preserves the exact CARLA
data-point timestamps. The project owns the maintenance and conformance
burden for this profile. Broader authorization, metadata search, curve/logical
filters, compression or additional writable targets require a separate contract
and security review; the accepted strict-role/QM boundary is not a general
actuator interface.

The MPL-2.0 obligations discussed above do not apply to the project code
because COVESA implementation artifacts are not redistributed. If VISSR code
or specification schemas are copied later, this decision must be revisited and
the applicable notices and source obligations added in the same change.
