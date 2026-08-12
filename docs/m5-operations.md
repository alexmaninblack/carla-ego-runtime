# M5 operations and acceptance

## Fixed route

The checked-in `config/m5_town10hd_route.json` defines the first repeatable M5
drive:

- map: `Carla/Maps/Town10HD_Opt`;
- start: recommended spawn point 40;
- destinations: spawn point 0, then spawn point 40;
- control source: CARLA BehaviorAgent with `normal` behavior;
- synchronous step: 0.05 seconds, paced in real time;
- expected routed distance on the pinned map: approximately 767.6 metres.

The route configuration pins CARLA commit
`385927b6ac5efaaa204b5b9853a7aaa5c5917428`. The controller rejects a different
map, client/server version mismatch, occupied hero role, invalid indices, short
legs, and invalid timing or networking values before driving.

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
the same runner by increasing `route.cycles`, `maximum_route_seconds`, or
`--repeat`; the manifest makes every result comparable without changing the
control/telemetry boundary.

## Apple Silicon acceptance record

The pinned native macOS baseline completed a visual route followed by two
consecutive headless restart runs on `Town10HD_Opt`. Each restart covered the
same 767.63 m route. The controller completed 3,260 frames in 163.01 seconds
and 2,555 frames in 127.76 seconds; route duration varies because BehaviorAgent
obeys live traffic lights and surrounding traffic.

The telemetry runtime published 3,264 and 2,558 frame-aligned VSS updates and
accepted 1,632 and 1,279 GNSS fixes, with zero GNSS rejections. In each run an
independent verified-TLS client received subscription data at the start and a
fresh frame at the end. VISS reported two accepted connections, two requests,
two subscription events, and zero protocol errors, dropped events, or
coalesced intervals per run.

Both controller and runtime returned exit code zero. After the second restart,
synchronous mode was disabled, no `hero` vehicle or GNSS actor remained, and
no process was listening on the VISS port. The detailed manifests and event
logs remain local and are intentionally excluded from the public repository.
