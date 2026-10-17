# ADR 0009 — Authenticated local external-control channel

Status: accepted

## Context

An independent client must control throttle, brake and steering without
coupling actuator safety to the telemetry runtime. A replaceable control-source
process owns synchronous ticks and the vehicle, while the C++ runtime observes
ticks and serves VISS telemetry.

VISS `set` is not a motion-control interface. Its current strict mTLS profile
permits only the accepted typed Brake/Tire QM advisory writes, not throttle,
brake or steering. Motion commands use a separate authenticated local channel,
keeping control ownership and timing independent of telemetry consumers.

## Decision

The external-control source is a separate Python tick-owner process. It owns
the ego vehicle, applies commands, and advances the 30 Hz CARLA simulation. The
existing C++ runtime remains a non-owning telemetry observer.

An independent local client connects through a Unix-domain stream socket. The
socket and token file are created in a short per-run temporary directory with
owner-only permissions and are removed during cleanup. This avoids the small
Unix-domain path limit when durable run artifacts use a descriptive path.
The base JSON-lines protocol is version 1. Negotiated mode handover and
scenario extensions retain its safety/ownership rules; the
[current external-control contract](../external-control-contract.md) owns
supported versions and the [handover decision](0010-live-handover-with-traffic-manager.md)
owns mode transitions.

The protocol has four actions:

- `acquire` authenticates with the per-run token and grants one client an
  exclusive session identifier;
- `command` carries a strictly increasing sequence number plus normalized
  throttle, brake, and steering values;
- `heartbeat` keeps the exclusive ownership lease alive but does not make an
  old actuator command fresh;
- `release` relinquishes ownership and immediately selects safe stop.

Throttle and brake are in `[0, 1]`; steering is in `[-1, 1]`, with negative
meaning left in CARLA's native control convention. A command that is missing,
expired, malformed, out of range, out of sequence, or associated with the
wrong session is never applied.

The safe state is zero throttle, full brake, and centred steering. It is
selected before acquisition, after release, immediately after disconnect, when
the command freshness deadline expires, or when the ownership heartbeat lease
expires. Heartbeat and command deadlines use monotonic time.

BehaviorAgent remains an independently selectable control source. This base
protocol does not silently transfer a live external-control session to BehaviorAgent; fallback
within the external-control source means a deterministic safe stop. Automatic
handover uses the separately accepted mode-transition contract; support for
that extension does not change version-1 fail-safe semantics.

## Consequences

- Loss of the external client cannot leave the last throttle command latched.
- Local OS permissions and a per-run secret authenticate the initial client;
  no reusable credential is committed or logged.
- Only one client can own control, and stale messages cannot replay an older
  command.
- VISS telemetry and its narrow QM advisory channel remain independent of
  control acquisition and release; neither grants motion-control ownership.
- Remote motion control is outside this local-channel contract. It requires
  a separately authenticated and encrypted transport plus a deployment threat review.
