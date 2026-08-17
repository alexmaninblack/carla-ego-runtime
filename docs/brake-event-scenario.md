<!-- Copyright (c) 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# Deterministic CARLA brake-event scenario

## Purpose and boundary

This workflow creates a visible, repeatable braking event for the Brake Health
demonstration while changing only the simulated vehicle side:

- CARLA renders the city, ego vehicle, stationary obstacle, and physical stop;
- `brake_event_scenario.py` is the sole synchronous simulation-clock and ego
  control owner;
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
the simulator and keeps the expanded engineering dashboard visible in its
Terminal window. `--dashboard-quiet` is reserved for unattended acceptance
runs where the same dashboard health is verified without visual rendering.

Use the same native runtime, CARLA Python environment, and loopback TLS material
as the existing M6.2 launcher:

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
