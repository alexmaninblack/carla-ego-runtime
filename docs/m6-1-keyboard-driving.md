# M6.1 keyboard driving and observable startup

## Control model

M6.1 keeps the M6 external-control contract unchanged. A small native macOS
AppKit panel captures key-down and key-up events only while its own window has
focus and streams normalized controls through the authenticated local M6
socket.

M6.2 later extends this panel and contract with live autopilot handover. The
manual-only M6.1 configuration and its acceptance record remain reproducible.

- Up ramps throttle toward 0.55.
- Down ramps brake toward 0.75 and immediately suppresses throttle.
- Left and Right ramp steering toward -0.55 and +0.55; release recentres it.
- Space selects safe stop.
- Enter explicitly arms or resumes control.
- Escape or closing the panel safely releases control and ends the session.
- Focus loss, bridge loss, and command timeout select zero throttle, full
  brake, and centred steering.

The panel is native Swift/AppKit code, not CARLA's Pygame manual-control
example. The launcher builds and ad-hoc signs a local app bundle from the
checked-in source when needed. Generated binaries, run artifacts, TLS files,
and local machine paths stay outside this public repository.

The on-screen arrow controls form the same cross as the physical arrow keys:
throttle above, brake below, and steering to either side. Arm/resume and safe
stop are complete labelled buttons rather than detached captions and colour
blocks.

## Operator session

The local wrapper calls `tools/launch_m6_1.py`. If CARLA is not reachable, the
launcher starts the configured Unreal Editor game window and Town10HD map. It
then starts the external-control tick owner, C++ telemetry runtime, independent
VISS probes, live terminal dashboard, and native keyboard panel.

The terminal remains visible beside CARLA and reports connection, data health,
simulation cadence, dashboard cadence, VISS latency, vehicle state, pedal and
steering values, and GNSS. The keyboard window intentionally starts in full-
brake safe stop and requires Enter before it acquires actuator ownership.

## Startup timeline

Each private run directory contains `startup-timeline.json`. It records elapsed
wall-clock time from the desktop launch through:

- launcher and native keyboard-app readiness;
- preflight and whether the run is cold or warm;
- Unreal process start when required;
- CARLA RPC/map readiness;
- vehicle spawn;
- first VSS frame;
- independent VISS verification;
- dashboard readiness;
- keyboard-window readiness;
- session finish and verified cleanup.

The authenticated socket and token are intentionally kept in a separate short
owner-only temporary directory. macOS limits Unix-domain socket path length,
so placing them under a long Application Support artifact path is not safe.
The launcher removes this temporary directory during session cleanup.

## Acceptance

Automated acceptance requires the existing M6 protocol suite, M6.1 launcher
tests, and native Swift typecheck to pass. The visual cold-start acceptance is
operator-observed: begin with CARLA fully stopped, use the desktop shortcut,
observe black-window/loading duration, drive with all four arrows, move focus
away to verify safe stop, resume with Enter, then close the keyboard window.
The resulting manifest must show clean runtime/controller exits, no control
owner, and removed socket/token; CARLA world settings and owned actors must also
be restored.

This acceptance completed on the pinned Apple Silicon baseline on 2026-08-12.
The operator confirmed the cold start, manual drive, live dashboard, safe-stop
behaviour, and clean exit. The resulting private artifacts report a completed
run, a stopped controller with no owner, and removed control credentials.
