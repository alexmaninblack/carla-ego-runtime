# M6 external-control operations and acceptance

## What M6 runs

M6 starts four deliberately separate processes around an already-running CARLA
server:

1. the external-control process owns the ego vehicle and the 30 Hz synchronous
   simulation clock;
2. the C++ runtime observes every tick, samples vehicle/GNSS state, and exposes
   the read-only TLS VISS endpoint;
3. an independent VISS client monitors telemetry health;
4. an independent control client acquires the private command channel, drives,
   releases, and deliberately disconnects during the bounded acceptance
   scenario.

The command channel is newline-delimited JSON over a local Unix-domain socket.
A fresh per-run token protects acquisition, and both files use owner-only
permissions. They are removed at shutdown. The public protocol is specified in
[External control contract v1](external-control-contract.md).

## Run the bounded acceptance

Build the native runtime first and keep CARLA open on the map selected by
`config/m6_town10hd_external_control.json`. Supply only local paths; certificate,
key, and run artifacts must remain outside Git.

```sh
python3 tools/run_m6.py \
  --config config/m6_town10hd_external_control.json \
  --runtime /path/to/carla-ego-runtime \
  --viss-client /path/to/carla-viss-client \
  --python /path/to/carla-python \
  --python-api-root /path/to/CARLA/PythonAPI/carla \
  --certificate /private/path/server-cert.pem \
  --private-key /private/path/server-key.pem \
  --run-root /private/path/to/m6-runs
```

Use `--headless --dashboard-quiet` for non-visual restart checks and
`--repeat 2` to require two complete sequential sessions. `--headless` hides
only the terminal dashboard; the CARLA editor continues to render normally.

## Acceptance sequence

The checked-in independent client performs these phases:

- straight throttle for 3 seconds;
- throttle plus a right turn for 2 seconds;
- heartbeat without a new actuator command, proving the 250 ms command deadline
  still selects safe stop;
- command recovery, explicit braking, and explicit release;
- a second acquisition followed by intentional transport disconnect.

The orchestrator accepts a run only when both VISS probes pass, the dashboard
remains live, the runtime and controller exit cleanly, and measured motion shows
at least 5 m travelled, at least 5 km/h reached, and no more than 0.5 km/h at
the end. Release, disconnect, command timeout, and shutdown all select zero
throttle, full brake, and centred steering.

## Local acceptance record

M6 was accepted on 2026-08-12 on the pinned Apple Silicon baseline. The final
motion-qualified session produced:

- 8.88 m measured travel, 8.61 km/h maximum speed, and 0.00 km/h final speed;
- 180 accepted commands across two authenticated acquisitions;
- one command timeout, one explicit release, and one intentional disconnect;
- zero rejected messages and no surviving control owner;
- a continuous measured 30.0 Hz simulation cadence and 4.0 dashboard events/s;
- 0.61 ms average and 0.8 ms maximum local VISS event latency;
- successful independent VISS probes before and after driving;
- clean removal of the vehicle, Unix socket, token, and prior synchronous world
  settings.

A visual run and two additional sequential restart runs completed before the
motion-qualified session. The Python protocol/tool suite and the complete CTest
suite are required to pass before tagging the release.

## Safety boundary

This is an authenticated local development channel, not a remotely exposed
drive-by-wire service. Do not replace the Unix socket with a network listener
without a separate threat model, mutual authentication, authorization policy,
rate limits, audit requirements, and network-loss safety analysis. VISS remains
read-only and must not be used as an implicit shortcut around that review.
