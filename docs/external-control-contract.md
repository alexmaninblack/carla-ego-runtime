# External control contract v3

## Native telemetry development slice — 2026-09-07

The operator authorized combining Driving Control and telemetry, followed by
an immediate live democtl trial; broad qualification and publication are deferred.
`carla-viss-client --monitor-json` emits one schemaVersion=1 UTF-8 JSON line
every 500 ms, at most 64 KiB, with source=gateway-viss, connection/state,
existing vehicle/exercise/physical-stop projections, metric text, advisory
availability and up to 128 bounded signal entries. No credentials enter samples.
The native Control application owns that single read-only child and consumes
its stdout pipe without recording samples. Terminal `--monitor` is retained.
The native display additionally expires its input after five seconds and
marks child exit disconnected. Neither condition changes vehicle commands.
Control ownership/focus/Safe Stop semantics remain on the existing bridge.
The runner no longer needs a Terminal or a second dashboard process. CARLA,
VM and Cloud interfaces are unchanged. This is not complete advisory support.

The in-car native dashboard presents vehicle telemetry only. Its drive mode is
the observed ActiveMode, while MOVING / STOPPING / STOPPED describes physical
movement (STOPPED at absolute speed <= 0.3 km/h). Missing/non-finite speed or a
reset reports MOTION UNKNOWN; stale/disconnected input reports NO FRESH DATA.
STOPPING requires observed SAFE_STOP with speed above that threshold. The
native display does not consume the engineering `stop` projection or show
platform terminology, installation state or update permission. AosCore's
authorization gate and the separate Cloud-only Platform Team view are unchanged.

### Fixed telemetry pages — accepted 2026-09-07

The native telemetry area uses Dashboard / Vehicle / Data pages, replacing the
expanding Engineering details section. Changing pages never resizes the window
or creates scrolling. Selected vehicle, freshness, speed and observed drive/motion
state remain visible on every page. Dashboard shows pedals and the two advisory
availability rows without duplicate footnotes. Vehicle groups steering, gear,
RPM and four wheel speeds around a top-view schematic. Data groups stream
metrics, coordinates, session/generation and the last event timestamp in UTC.

Telemetry draws in actual screen points, not the old scaled 620x600 canvas.
The combined window's minimum content size is 900x470; the existing composed
914-pixel-wide window still fits. Native segmented controls expose the page
choices to accessibility and return driving keyboard focus after selection.
The Control side, source transport, data freshness rules and Safe Stop behavior
are unchanged. Only the native UI target needs recompilation for this change.

## Selected-vehicle external connectivity — 2026-09-07

Driving Control has one disconnect/reconnect button outside the telemetry
area. The interactive runner accepts an optional trusted
`--connectivity-command` JSON argv prefix supplied by Demo Control; native
Control receives it as its fourth process argument. It invokes the fixed
`status`, `off`, or `on --target test|production` suffix without a shell.
Target comes from the latest successful status, not an independent selector.

Network policy and ownership belong to Demo Control, not to the native app.
ON/OFF reports that owned filter setting, not Aos Cloud reachability. Status
is read asynchronously every five seconds with bounded output/deadline;
unavailable/expired input shows UNKNOWN or triggers a read before mutation.
One child operation runs at a time; control/telemetry processing is separate.
Closing waits for the in-flight operation but does not reconnect the VM.
The explicit CLI restore works even after simulation stop. No network state
is added to the in-car telemetry or its VISS contract.

## Interactive session lifetime — 2026-09-07

The interactive runner used by `democtl simulation start` passes
`--until-stopped` to its existing Controller. This disables only the total
session-duration cap, including the former 3600-second exit. The run manifest
records `session_lifetime: until_stopped`; the unchanged configuration's
`maximum_session_seconds` still applies to bounded, noninteractive runs.
Explicit stop, signal handling, command/ownership expiry, disconnect Safe Stop
and startup/shutdown timeouts retain their existing behavior. No VM image or
Cloud configuration change is required.

## Engineering display additions — 2026-09-07

The VISS monitor renders read-only drive/reset context, a shortened exercise
fingerprint and the physical Safe Stop observation alongside existing vehicle,
wheel/slip and GNSS telemetry. It does not evaluate the runtime-owned twelve-
frame FOTA authorization gate. `--demo-journal` optionally supplies the existing
Demo Control journal for a fixed Test/Production/Not assigned label; its source
run ID must match the VISS run, and no private journal fields are displayed.

Rendering continues on a 500-ms timer while the VISS read is pending. More than
five seconds without a received/advancing frame labels all values stale and
the physical stop state unknown. Disconnect is displayed explicitly. These
display semantics do not change source timestamps or the runtime freshness
policy. Driver Advisory has separate Brake and Tire rows, both explicitly
UNAVAILABLE until the actual Gateway advisory implementation is connected.
No request or applied advisory status is synthesized from telemetry.

## Demo Orchestrator Safe Stop and Reset

### Test scene recovery and maneuvers — accepted 18 September 2026

P5 adds a separate trusted `--scene-command` argv prefix to the existing native
runner. Driving Control invokes only Demo Control's Test `simulation
return-to-road` or `simulation exercise brake|tire` action without a shell.
No new controller/tick owner, guest command, model Reset or advisory Reset is
introduced. Scene preparation is labelled before a maneuver starts. The
native control socket/heartbeat and telemetry reader continue independently;
held manual keys are cleared and driving controls are blocked during the
operation, with Safe Stop still available. One pending operation is reconciled
by its original journal identity after uncertain output.

Return to road reuses safe_stop/reset/manual_ready/release_manual. The tick
owner checks the configured known-good spawn is a Driving lane, within two
metres of its projected centre and ten degrees of lane direction, with bounded
pitch/roll and conservative vehicle/walker/prop clearance. Unknown geometry or
occupied placement is rejected before teleportation. Existing actor, scene,
Unit assignment, models and advisory remain. Successful relocation still emits
the existing reset-generation/discontinuity frame facts, not invented samples.

The local orchestration status adds `roadRecoverySupported`, `resetError` and
`frame.roadReady`. A rejected placement has `RESET_FAILED`, no successful reset
generation, and an idempotent repeated reset response. Only a fresh confirmed
stopped frame permits release into Safe Stop after that failure. A successful
placement requires advancing stationary road-confirmed frames for at least
half a second before stationary Manual. It never resumes Autopilot. Live
discontinuity/product and native UI qualification remain separate from tests.

### Initial stationary Manual readiness — authorized 2026-09-06

The operator explicitly authorized a narrowly scoped command-timeout exception
for initial demo readiness. `manual_ready` requires this operation's confirmed
post-reset Safe Stop frame and an existing native operator session. It applies
zero throttle, full brake and centered steering in Manual, keeps the driving
interlock, then reports `MANUAL_READY` only from a real completed stationary
Manual frame. `release_manual` requires that fresh frame before unlocking.
Demo Control uses this only for the first Test attachment, while both source
paths are still blocked; normal vehicle switching keeps its Safe Stop behavior.

The stationary `manual_ready` command has no 250-ms actuator deadline because
it cannot request motion. The first operator actuator command restores normal
command expiry. Ownership timeout, disconnect, release, explicit Safe Stop and
controller failure still stop the car. No requested mode is reported as applied
before CARLA completes the frame, and no AosCore gate or VDP evidence is bypassed.

The operator authorized this host-side extension on 2026-09-05. Protocol v3
adds `action: "orchestrate"` on the existing private socket. Exact fields are
`version`, `action`, `requestId`, `token`, `operation` and canonical UUID
`operationId`. The protected per-run token is not a Cloud or VISS identity.

Operations are `safe_stop`, `status`, `reset` and `release`. Safe Stop takes
an interlock, preserving the native UI session but rejecting driving until
release. `STOPPING` becomes `SAFE_STOP` only after a real completed stopped
CARLA frame with full brake. Another operation cannot replace the holder;
duplicates cannot repeat its side effects. Response loss/disconnect leaves
the interlock held, never automatically resuming driving.

Reset requires the same operation and a fresh stopped frame. The existing
tick owner resets the ego to its initial spawn without entering Scenario.
`RESETTING` becomes `RESET` only after a real post-reset frame and exactly one
new reset generation. This is supported for the plain manual/autopilot scene,
not the hybrid obstacle scene. Duplicate resets do not reset twice.

Status reports held operation, phase, freshness and completed frame facts.
Release requires a fresh stopped frame and leaves Safe Stop active. The
Orchestrator must prove source detachment before reset; Controller completion
does not prove source exclusivity or VDP readiness. No token is returned in
status, snapshots or logs.

The interactive runner used by `democtl simulation stop` requests graceful
child shutdown and disables its inherited SIGKILL fallback. Timeout remains
an incomplete shutdown to reconcile, not permission to force-kill a child.
Other M5 callers retain their existing default shutdown policy.

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

The owned ego blueprint explicitly sets `sticky_control=false` before spawn.
CARLA's client-side ApplyControl cache is independent of Traffic Manager's
batch controls: without this setting, a repeated full-brake request after
autopilot can be suppressed even though the actual actor has throttle applied.
Every Controller brake request must reach CARLA. A blueprint without that
attribute is rejected before spawn; there is no silent compatibility fallback.
This changes neither the tick owner nor the Safe Stop evidence thresholds.

The requested protocol mode is not automatically the applied vehicle mode.
Ordinary Manual mode remains applied `SAFE_STOP` until the first valid actuator
command. A separately authorized initial `manual_ready` state holds zero throttle and
full brake while awaiting the operator. In that stationary state only, the
native UI emits neither neutral actuator commands nor a focus-loss mode
change. Selecting Manual or Autopilot ends UI waiting; explicit Safe Stop,
disconnect and ownership timeout retain their normal effects. Ordinary
manual driving retains its focus-loss stop and command deadline.
A command timeout likewise returns the applied mode to `SAFE_STOP` while the
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
of the authenticated operator-control stream: it is one connected,
non-blocking `AF_UNIX`/`SOCK_STREAM` record after each successful real
`world.tick`. Every UTF-8 JSON body is preceded by one unsigned big-endian
32-bit body length.
The record contains only `schemaVersion`, run and ego identity, the returned
CARLA frame and simulation time, the actually applied mode, transition state,
control and reset generations, reset-in-progress and one-frame reset
discontinuity. It never contains a command token, session, operator identity,
Safe Stop conclusion or history.

The C++ process binds and listens before the startup gate, requires an
owner-only `0700` directory and `0600` socket, and accepts exactly one
connection for the run. Both endpoints verify the peer effective UID:
`getpeereid` on Darwin and `SO_PEERCRED` on Linux. The receiver retains at most
one bounded partial frame and rejects zero or greater-than-4096 body lengths
before JSON parsing. The producer makes one non-blocking connection attempt
and one non-blocking write attempt per completed frame. Backpressure, a
partial write, missing receiver, EOF, disconnect or process shutdown makes the
channel unavailable, never blocks CARLA ticks and never changes controller
behavior. There is no reconnect, replay or history protocol within the run.
The unprivileged native C++ macOS suite proves the successful same-UID
`getpeereid` receiver path. A real receiver-side wrong-UID Darwin negative
requires a second effective UID and remains an environment qualification; no
privileged peer or mocked credential is presented as native receiver evidence.
The Python sender unit test separately injection-tests its rejection branch and
is not OS-level wrong-UID proof.

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
