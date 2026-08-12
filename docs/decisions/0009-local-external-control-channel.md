# ADR 0009 — Authenticated local external-control channel

Status: accepted for M6 implementation

## Context

M6 needs an independent client to control throttle, brake, and steering without
coupling actuator safety to the telemetry runtime. The M5 architecture already
assigns synchronous ticks and vehicle ownership to a replaceable control-source
process, while the C++ runtime observes ticks and serves read-only VISS
telemetry.

VISS `set` is not used for M6 control. The current VISS profile deliberately
has no client authentication and exposes only sensor and attribute nodes.
Making its network endpoint writable would combine authentication, actuator
authorization, clock ownership, and telemetry compatibility in one change.

## Decision

The external-control source is a separate Python tick-owner process. It owns
the ego vehicle, applies commands, and advances the 30 Hz CARLA simulation. The
existing C++ runtime remains a non-owning telemetry observer.

An independent local client connects through a Unix-domain stream socket. The
socket and token file are created inside the private run directory with owner-
only permissions and are removed during cleanup. Each JSON-lines message uses
control contract version 1.

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

BehaviorAgent remains an independently selectable control source. M6 does not
silently transfer a live external-control session to BehaviorAgent; fallback
within the external-control source means a deterministic safe stop. A later
automatic-driving handover requires its own state-transition and route-rejoin
acceptance work.

## Consequences

- Loss of the external client cannot leave the last throttle command latched.
- Local OS permissions and a per-run secret authenticate the initial client;
  no reusable credential is committed or logged.
- Only one client can own control, and stale messages cannot replay an older
  command.
- VISS remains compatible, read-only, and continuously available during
  control acquisition and release.
- Remote control is outside M6. It requires a separately authenticated and
  encrypted transport plus a deployment threat review.
