<!-- Copyright (c) 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# Deterministic CARLA brake-event scenario

## Purpose and boundary

This workflow creates a visible, repeatable braking event for the Brake Health
demonstration while changing only the simulated vehicle side:

- CARLA renders the city, ego vehicle, stationary obstacle, and physical stop;
- the interactive workflow reuses `external_control_controller.py` as the sole
  synchronous simulation-clock and ego-control owner;
- `carla-ego-runtime` observes the same frames, exposes VISS telemetry, and
  drives the chase camera;
- the engineering dashboard shows vehicle motion, pedals, and live four-wheel
  dynamics;
- the run records a deterministic result and cleanup evidence.

The workflow does **not** install or modify AosVM, KUKSA, the Vehicle Data
Provider, the Brake Health service, authorization, or AosCloud state. It
therefore remains usable while those layers are still being designed.

## Scenario state machine

The checked-in `stationary-obstacle-braking-v1` configuration uses these
phases:

1. `PREFLIGHT` validates the CARLA version, map, configuration, and actor roles.
2. `SPAWN` creates the ego vehicle, a physics-disabled stationary vehicle as
   the obstacle, and an ego collision sensor.
3. `ACCELERATE` reaches the configured 20 km/h target.
4. `STABILIZE` holds the target for a bounded interval.
5. `APPROACH` follows the deterministic lane branch and maintains speed.
6. `BRAKE` applies the configured brake command at the configured physical
   obstacle gap.
7. `HOLD` requires a stable stop for consecutive frames and keeps the brake
   applied so the result remains visible.
8. `EVALUATE` checks collision count, brake-onset speed, peak deceleration, and
   final obstacle gap.
9. `CLEANUP` destroys only scenario-owned actors and restores the original
   world settings.

The obstacle exists before motion starts. Nothing is teleported into the ego
vehicle's path during the controlled segment. Lane-branch selection is stable:
the controller prefers forward-heading continuity and uses road/lane metadata
as deterministic tie-breakers.

## Interactive and qualification profiles

`CARLA Brake Event.app` uses
`config/brake_event_hybrid_town10hd.json`. The M6.2 native panel offers:

- **Start/Restart Scripted Scenario** or S;
- **Manual Control** or M/Enter with arrow-key control;
- **Autopilot** or A;
- **Safe Stop** or Space;
- session cleanup through Escape or closing the panel.

All modes keep the same ego actor, VISS endpoint, dashboard, camera, and tick
owner. Selecting Manual during a scripted attempt records `ABORTED`; it does
not produce a misleading automatic `PASS` or `FAIL`. Selecting the scripted
mode again resets the ego state and starts a new attempt. A completed attempt
selects safe stop but keeps the session alive so the operator can restart it or
continue manually.

`config/brake_event_town10hd.json` and `launch_brake_event_demo.py` remain the
unattended qualification profile. They run exactly one scripted attempt,
record its acceptance result, clean up, and optionally suppress dashboard
rendering with `--dashboard-quiet`.

## Engineering telemetry

The dashboard retains the original speed, acceleration, steering, pedal, gear,
RPM, and GNSS views and adds a four-column `FL / FR / RL / RR` wheel table:

- standard VSS wheel angular speed in rad/s;
- standard VSS linear wheel speed in km/h;
- simulator-specific live Chaos longitudinal slip;
- simulator-specific live Chaos lateral slip angle in degrees.

The first increment deliberately does not fabricate brake temperature, pad
wear, remaining useful life, or a failure prediction. Those require a reviewed
simulation model and explicit source-provenance rules. They can be added later
without changing the deterministic obstacle controller.

## Acceptance evidence

Every run directory contains:

- the effective immutable configuration;
- structured process events;
- actor IDs and scenario phase/status;
- brake-onset speed and frame count;
- peak longitudinal deceleration;
- minimum and final obstacle gap;
- collision count and frames;
- a top-level `PASS` or `FAIL` plus exact failure reasons;
- runtime, controller, and dashboard health in the final manifest.

`PASS` requires no collision, a stable final gap inside the configured safety
window, braking at the target speed within tolerance, and sufficient measured
deceleration. A later repeatability gate will execute the accepted calibration
multiple times with a strict reset between runs.

## Operator command

The macOS launcher installer creates `CARLA Brake Event.app`. Opening it starts
the hybrid session, native control panel, and expanded engineering dashboard in
Terminal. Closing the panel cleans the session. CARLA is also closed when that
session started it. The desktop application also adopts and closes a compatible
already-running `CarlaUnreal` process after verifying its project path and RPC
port. This gives Escape and panel close one consistent full-demo lifecycle.

For an unattended single-run qualification, use the same native runtime, CARLA
Python environment, and loopback TLS material as the existing M6.2 launcher:

```sh
python tools/launch_brake_event_demo.py \
  --config config/brake_event_town10hd.json \
  --runtime /path/to/build/carla-ego-runtime \
  --viss-client /path/to/build/carla-viss-client \
  --python /path/to/carla-python \
  --python-api-root /path/to/CARLA/PythonAPI/carla \
  --certificate /private/path/server-cert.pem \
  --private-key /private/path/server-key.pem \
  --run-root /private/path/brake-event-runs \
  --unreal-editor /path/to/UnrealEditor \
  --uproject /path/to/CarlaUnreal.uproject \
  --startup-map /Game/Carla/Maps/Town10HD_Opt \
  --unreal-argument=-game \
  --unreal-argument=-windowed \
  --unreal-argument=-quality-level=Low \
  --unreal-argument=-nosound \
  --unreal-argument=-carla-rpc-port=2000
```

Local paths, TLS material, run artifacts, and Unreal binaries remain outside
the public repository.
