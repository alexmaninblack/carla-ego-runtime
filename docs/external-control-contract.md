# External control contract v1

## Transport and access

The M6 development profile uses newline-delimited JSON over a Unix-domain
stream socket. Both the socket and token file are local run artifacts with
owner-only access. Every request contains `version: 1`, `action`, and a unique
`requestId`. Every response repeats the action and request ID, adds an ISO 8601
UTC `ts`, and contains either `status: "ok"` or a structured `error`.

## Acquire

```json
{"version":1,"action":"acquire","requestId":"a1","clientId":"demo-driver","token":"<per-run-token>"}
```

A successful response returns an unguessable `sessionId`. Only one session may
own control. Authentication failures and busy ownership are indistinguishable
to callers except for the documented error code.

## Command

```json
{"version":1,"action":"command","requestId":"c1","sessionId":"<session>","sequence":1,"throttle":0.25,"brake":0.0,"steering":0.0}
```

`sequence` is a positive integer and must strictly increase within a session.
Throttle and brake range from 0 to 1. Steering ranges from -1 (left) to 1
(right). Simultaneous non-zero throttle and brake are rejected. A valid command
refreshes both command freshness and the ownership lease.

## Heartbeat and release

```json
{"version":1,"action":"heartbeat","requestId":"h1","sessionId":"<session>"}
{"version":1,"action":"release","requestId":"r1","sessionId":"<session>"}
```

Heartbeat retains ownership but never extends actuator-command freshness.
Release selects safe stop before acknowledging success.

## Safety deadlines

The checked-in M6 profile uses a 250 ms command freshness deadline and a 1,000
ms ownership lease. Safe stop is zero throttle, full brake, and centred
steering. It is applied on startup, release, disconnect, either deadline, and
shutdown. The server reports state transitions and counters in the private run
manifest without recording the token.

## Errors

Errors use stable codes: `bad_request`, `unauthorized`, `control_busy`,
`invalid_session`, `invalid_sequence`, and `invalid_command`. Invalid input
does not refresh either deadline and is not applied to CARLA.

## Lifecycle and telemetry separation

The external-control process is the sole owner of its spawned ego vehicle and
CARLA's synchronous tick while selected. It refuses to start if the configured
hero already exists, restores the previous world settings, and destroys only
its own actor. BehaviorAgent remains available through its separate M5
configuration; there is no automatic hand-off between control sources.

The C++ runtime attaches as a non-owning observer. VISS remains a read-only
telemetry interface throughout acquisition, driving, timeout, release, and
disconnect. Socket paths, token files, and token values are never part of the
public VISS tree.
