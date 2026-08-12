# External control contract v2

## Transport and access

The M6 development profile uses newline-delimited JSON over a Unix-domain
stream socket. The socket and per-run token file have owner-only access. Every
request contains `version`, `action`, and a unique `requestId`. Every response
repeats those fields, adds an ISO 8601 UTC `ts`, and contains either
`status: "ok"` or a structured `error`.

Version 2 adds explicit live drive modes. The server continues to accept
version 1 clients: acquiring a v1 session selects the legacy manual mode, and
its `command`, `heartbeat`, and `release` semantics are unchanged.

## Acquire

```json
{"version":2,"action":"acquire","requestId":"a1","clientId":"demo-driver","token":"<per-run-token>"}
```

A successful response returns an unguessable `sessionId`. Only one session may
own control. A v2 session begins in `safe_stop`; choosing a driving mode is a
separate, explicit action.

## Drive modes

```json
{"version":2,"action":"set_mode","requestId":"m1","sessionId":"<session>","mode":"manual"}
{"version":2,"action":"set_mode","requestId":"m2","sessionId":"<session>","mode":"autopilot"}
{"version":2,"action":"set_mode","requestId":"m3","sessionId":"<session>","mode":"safe_stop"}
```

- `manual` accepts normalized actuator commands from the session owner.
- `autopilot` lets the configured CARLA Traffic Manager control the same ego
  actor while the external controller continues to own simulation ticks.
- `safe_stop` disables automatic control and applies zero throttle, full brake,
  and centred steering.

Selecting the current mode again is idempotent and does not interrupt control.
Autopilot activation is rejected unless the vehicle is close to a driving lane
and aligned with its direction. Switching from autopilot to manual disables
Traffic Manager before accepting manual actuation; the controller blends from
the last automatic control for the configured handover interval.

## Manual command

```json
{"version":2,"action":"command","requestId":"c1","sessionId":"<session>","sequence":1,"throttle":0.25,"brake":0.0,"steering":0.0}
```

`sequence` is a positive integer and must strictly increase for the complete
session, including across mode changes. Throttle and brake range from 0 to 1.
Steering ranges from -1 (left) to 1 (right). Simultaneous non-zero throttle and
brake are rejected. Commands outside `manual` mode are rejected and never
applied. A valid command refreshes command freshness and the ownership lease.

## Heartbeat and release

```json
{"version":2,"action":"heartbeat","requestId":"h1","sessionId":"<session>"}
{"version":2,"action":"release","requestId":"r1","sessionId":"<session>"}
```

Heartbeat retains ownership in every mode. It never extends manual actuator-
command freshness. Release selects safe stop before acknowledging success.

## Safety deadlines

The checked-in profile uses a 250 ms manual-command deadline and a 1,000 ms
ownership lease. A manual command timeout selects safe stop but retains the
session so the operator can recover. An ownership timeout, client disconnect,
release, window close, process shutdown, or controller failure disables
autopilot, applies safe stop, and drops ownership where applicable. Automatic
mode is not subject to the manual-command deadline, but it requires heartbeats.

## Errors

Stable error codes are `bad_request`, `unauthorized`, `control_busy`,
`invalid_session`, `invalid_sequence`, `invalid_command`, `invalid_mode`, and
`mode_unavailable`. Invalid input does not refresh safety deadlines or reach
CARLA.

## Lifecycle and telemetry separation

The external-control process owns its spawned ego vehicle and CARLA's
synchronous tick for the complete session. Manual, automatic, and stopped modes
therefore do not replace the actor, reset the scene, or interrupt VISS. The C++
runtime remains a non-owning telemetry observer, and VISS remains a read-only
interface. Socket paths, token files, and token values never enter the public
VSS tree.

BehaviorAgent remains available through the separate M5 configuration. M6.2
does not start a second BehaviorAgent tick owner; live automatic driving uses
Traffic Manager inside the existing external-control process.
