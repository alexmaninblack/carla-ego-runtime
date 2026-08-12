# M5 operations and acceptance

## Fixed route

The checked-in `config/m5_town10hd_route.json` defines the first repeatable M5
drive:

- map: `Carla/Maps/Town10HD_Opt`;
- start: recommended spawn point 40;
- destinations: spawn point 0, then spawn point 40;
- control source: CARLA BehaviorAgent with `normal` behavior;
- synchronous step: 1/30 second, paced in real time;
- expected routed distance on the pinned map: approximately 767.6 metres.

The route configuration pins CARLA commit
`385927b6ac5efaaa204b5b9853a7aaa5c5917428`. The controller rejects a different
map, client/server version mismatch, occupied hero role, invalid indices, short
legs, and invalid timing or networking values before driving.

The same 1/30-second interval is passed into BehaviorAgent's longitudinal and
lateral PID controllers. If one frame misses a complete period, the tick pacer
starts a fresh period from that completion instead of issuing back-to-back
catch-up frames. This keeps the visual cadence smooth without changing the
deterministic simulation step.

## Build the matching Python API on Apple Silicon

Create a local virtual environment outside this repository and install the
BehaviorAgent dependencies:

```sh
python3 -m venv /path/to/carla/.venv-m5
/path/to/carla/.venv-m5/bin/python -m pip install \
  -r /path/to/carla-ego-runtime/tools/requirements-m5.txt
```

Configure the pinned CARLA checkout with `BUILD_PYTHON_API=ON` and the virtual
environment as `Python3_ROOT_DIR`, then build `carla-python-api`. Install the
generated arm64 wheel into that same environment. No Python, CARLA, or Unreal
binary is committed to this public repository.

## Run

After CARLA is listening and a TLS certificate exists outside the repository:

```sh
/path/to/.venv-m5/bin/python tools/run_m5.py \
  --config config/m5_town10hd_route.json \
  --runtime /path/to/build/carla-ego-runtime \
  --viss-client /path/to/build/carla-viss-client \
  --python /path/to/.venv-m5/bin/python \
  --python-api-root /path/to/carla/PythonAPI/carla \
  --certificate /private/path/server-cert.pem \
  --private-key /private/path/server-key.pem \
  --run-root /private/path/carla-m5-runs
```

Use `--repeat 2 --headless` for a restart test. `--headless` disables only the
spectator camera and exposure command; Unreal rendering remains controlled by
the simulator.

## Integrated M5.1 operator launcher

`tools/launch_m5.py` provides one operator-facing session. If CARLA is already
available it is reused; otherwise the launcher may start a configured Unreal
Editor command. Startup progress distinguishes simulator loading, CARLA
readiness, vehicle preparation, live VSS telemetry, independent TLS
verification, and dashboard readiness. Local Unreal, certificate, and build
paths are passed on the command line and remain outside the repository.

The launcher opens the routed drive and VSS health dashboard together. One
Ctrl-C requests an orderly stop. The launcher stops only processes that it
started, while the existing M5 gates guarantee that the telemetry runtime
removes its GNSS sensor before the controller removes the vehicle and restores
the world settings. A per-run-root lock prevents two local launchers from
owning the same session.

The dashboard reports:

- verified TLS/VISS connection state;
- live/stale data health;
- simulation cadence derived from VSS frame and simulation-time deltas;
- dashboard event delivery cadence;
- local event latency derived from the VISS event timestamp;
- the cumulative number of received subscription events.

These are operator diagnostics, not additional VSS signals. The measured
delivery rate follows `runtime.dashboard_period_ms` and is intentionally lower
than the 30 Hz simulation cadence.

## Artifacts and success criteria

Each run directory contains:

- `manifest.json`: redacted command, configuration, versions, route length,
  frame count, timings, exit codes, and final status;
- `events.jsonl`: structured events from the orchestrator, controller, runtime,
  and independent VISS probes;
- `controller-status.json`: atomic lifecycle and route result.

Success requires both route legs, the first published VSS frame before the
startup gate opens, independent VISS subscription data near the start, an
independent VISS read at the end, clean process exits, restored world
settings, and removal of the owned vehicle and GNSS actor. Runtime TLS paths
are redacted from the manifest. The local run directory is ignored by Git.

## Endurance

The first M5 acceptance route is a bounded endurance unit. Longer tests reuse
the same runner with `--route-cycles N` or `--repeat`; the cycle override scales
the safety timeout, and the effective configuration is stored beside the
manifest. The manifest makes every result comparable without changing the
control/telemetry boundary. Fourteen cycles are used for the M5.1 30-minute
Apple Silicon endurance acceptance.

## Visual cadence selection

The M5 physics cadence is 30 Hz. A local Apple Silicon comparison ran the same
rendered scene for 800 frames at 20, 30, and 40 Hz. Both 20 and 30 Hz completed
without a missed frame deadline. At 40 Hz, 92 of 800 frames exceeded the 25 ms
budget and the worst frame took about 66 ms. The 30 Hz configuration therefore
improves visible motion while retaining real-time headroom for the spectator
camera, telemetry, and traffic simulation.

## Apple Silicon acceptance record

The pinned native macOS baseline completed the full route with the spectator
camera at 30 Hz, followed by two consecutive headless restart runs on
`Town10HD_Opt`. Every run covered the same 767.63 m route. The visual run
completed 4,295 frames in 143.17 seconds. The restart runs completed 4,877
frames in 162.63 seconds and 4,236 frames in 141.20 seconds; route duration
varies because BehaviorAgent obeys live traffic lights and surrounding traffic.

The telemetry runtime published 4,300 VSS updates during the visual run and
4,881 and 4,240 frame-aligned VSS updates during the restart runs. It accepted
1,434, 1,627, and 1,414 GNSS fixes respectively, with zero GNSS rejections. In
each run an independent verified-TLS client received subscription data at the
start and a fresh frame at the end, with no protocol errors or dropped events.

Both controller and runtime returned exit code zero. After the second restart,
synchronous mode was disabled, no `hero` vehicle or GNSS actor remained, and
no process was listening on the VISS port. The detailed manifests and event
logs remain local and are intentionally excluded from the public repository.

## M5.1 Apple Silicon acceptance record

The integrated launcher was accepted through a visual desktop launch, an
operator-interrupt cleanup run, two consecutive full-route restarts, and a
14-cycle endurance drive. The endurance drive covered 10,746.82 m over 28 route
legs and ran for 1,920.84 seconds (32.01 minutes). It published 57,520
frame-aligned VSS state updates, accepted 19,195 GNSS fixes with zero
rejections, and delivered 7,679 dashboard subscription events with zero
protocol errors, dropped events, or coalesced intervals.

The two final restart runs covered 767.63 m each in 141.55 and 141.03 seconds.
Dashboard health reported `CONNECTED` and `LIVE` throughout both runs. The
measured simulation cadence was exactly 30.0 Hz in every collected sample;
dashboard delivery was 4.0 events/s. Average local VISS event latency was 0.70
and 0.67 ms, and the worst observed latency was 1.9 ms.

Both restart runs and the endurance run returned zero from the controller and
telemetry runtime, passed independent TLS/VISS start and end probes, restored
asynchronous world settings, removed the owned vehicle and GNSS sensor, and
left no VISS listener. The desktop launcher reused an already-open Unreal
instance and did not close it after one operator stop.
