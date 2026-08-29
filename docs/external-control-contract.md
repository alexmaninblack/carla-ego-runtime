# External control contract v3

## Transport and access

The M6 development profile uses newline-delimited JSON over a Unix-domain
stream socket. The socket and per-run token file are placed in a short,
owner-only temporary runtime directory because Unix-domain paths have a small
platform limit. Durable manifests and logs remain in the private run-artifact
directory. Every request contains `version`, `action`, and a unique
`requestId`. Every response repeats those fields, adds an ISO 8601 UTC `ts`,
and contains either `status: "ok"` or a structured `error`.

Versions 2 and 3 add explicit live drive modes and the hybrid Scenario mode.
The server continues to accept version 1 clients: acquiring a v1 session
selects the legacy manual request, and its `command`, `heartbeat`, and
`release` semantics are unchanged.

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

The requested protocol mode is not automatically the applied vehicle mode.
Manual mode remains applied `SAFE_STOP` until the first valid actuator command;
a command timeout likewise returns the applied mode to `SAFE_STOP` while the
session remains available for recovery. Each non-idempotent requested or
controller-forced applied-mode transition increments the bounded control
generation. The controller never reports a requested mode as applied before
the corresponding CARLA operation succeeds.

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

## Controller-to-Gateway frame facts

`run_m6.py` creates a second, short per-run Unix socket path in the same `0700`
runtime directory. It passes an explicit shared run ID and this facts path to
the Python tick owner and the C++ observer. The facts transport is independent
of the authenticated operator-control stream: it is one non-blocking
`AF_UNIX`/`SOCK_DGRAM` JSON record after each successful real `world.tick`.
The record contains only `schemaVersion`, run and ego identity, the returned
CARLA frame and simulation time, the actually applied mode, transition state,
control and reset generations, reset-in-progress and one-frame reset
discontinuity. It never contains a command token, session, operator identity,
Safe Stop conclusion or history.

The C++ process binds the datagram receiver before the startup gate, requires
an owner-only `0700` directory and `0600` socket, verifies Linux kernel peer
credentials and pins the first valid same-run/same-ego producer PID. The 4096
byte maximum and `MSG_TRUNC` are enforced before JSON parsing. Send
backpressure, a missing receiver or process shutdown increments telemetry-loss
evidence in the controller and never blocks CARLA ticks or changes controller
behavior.

The C++ observer accepts only a closed version-1 record with exact types and
enums. It joins facts to physical telemetry only when frame ID and binary
simulation time both match. At most four unmatched records per side remain for
250 ms of host-monotonic residence. Wrong identity, malformed, duplicate,
out-of-order, generation-regressing, expired or capacity-evicted input leaves
all six control/reset VSS facts absent for that physical frame; no last-known
value is reused. `controller-status.json` remains run evidence and is never
polled as a vehicle-state transport.

A canonical reset emits no fabricated frame while the operation blocks. Reset
generation advances only when a real completed post-reset frame exists; that
first record carries `Reset.InProgress=false` and
`Reset.Discontinuity=true`, and the next completed record clears the
discontinuity. A failed transition attempts one real full-brake frame with
`ActiveMode=SAFE_STOP` and `TransitionState=FAILED`; if CARLA cannot complete
that frame, no success or failure frame is invented.

BehaviorAgent remains available through the separate M5 configuration. M6.2
does not start a second BehaviorAgent tick owner; live automatic driving uses
Traffic Manager inside the existing external-control process.
