# CARLA Ego Runtime

A native C++ runtime for one instrumented ego vehicle in the
[CARLA simulator](https://github.com/carla-simulator/carla). It will expose
vehicle telemetry through the COVESA Vehicle Information Service Specification
(VISS).

> [!IMPORTANT]
> The local M6 path is implemented. Either the routed BehaviorAgent or an
> authenticated external-control process owns the ego vehicle and synchronous
> clock while the C++ runtime observes every frame and exposes telemetry through
> a TLS-only VISS 3.1 endpoint.
> Binding outside loopback remains an explicit deployment and security step.

## Current capabilities

- configurable CARLA RPC host, port, and timeout;
- exact LibCarla/server version validation by default;
- selection of an existing vehicle by `role_name`;
- deterministic spawn fallback using a configured blueprint and map spawn
  points;
- signal-aware bounded test lifetime;
- ownership tracking that never destroys a pre-existing vehicle;
- designated synchronous tick ownership with restoration of prior world
  settings on exit;
- optional wall-clock pacing for smooth real-time visualization;
- optional synchronous Traffic Manager autopilot and chase-camera demo mode;
- frame-aligned speed, acceleration, pedal, steering, gear, RPM, and four-wheel
  Chaos dynamics sampling;
- an attached 10 Hz GNSS sensor with source-frame ordering, freshness checks,
  and independent data-point timestamps;
- transport-independent normalization and VSS 6.0 projection;
- a bounded latest-value signal store and project-owned simulation overlay;
- a TLS 1.2+ VISS 3.1 endpoint with the mandatory `VISSv3` subprotocol;
- VISS Get, time-based Subscribe/Unsubscribe, and read-only Update errors;
- bounded per-client delivery with connection, error, event-drop, and
  coalescing metrics;
- a small independent TLS VISS client for acceptance tests;
- a dependency-free build mode for CLI tests and documentation CI;
- a native build mode against an installed `Carla::carla-client` package;
- a deterministic `Town10HD_Opt` BehaviorAgent route with a replaceable
  process boundary shared by the selectable M6 external-control source;
- validated M5 JSON configuration, structured event logs, redacted run
  manifests, independent VISS probes, and restart testing;
- an authenticated local M6 control contract with exclusive ownership,
  monotonic command sequences, independent command/ownership deadlines, and
  full-brake safe stop on timeout, release, disconnect, or shutdown;
- live M6.2 switching among manual control, synchronous Traffic Manager
  autopilot, and safe stop without replacing the vehicle or interrupting VISS.
- a deterministic stationary-obstacle braking scenario with a single tick
  owner, explicit phases, collision monitoring, machine-readable acceptance,
  and the same live engineering dashboard.

Run `carla-ego-runtime --help` for all connection and vehicle options.

## Initial telemetry scope

The initial runtime milestones intentionally use only low-cost vehicle state
and GNSS:

- vehicle speed;
- longitudinal, lateral, and vertical acceleration;
- simulated accelerator and brake pedal positions;
- equivalent front-axle steering angle;
- current gear and engine speed;
- GNSS latitude, longitude, and altitude;
- simulation run, frame, and time metadata;
- front-left, front-right, rear-left, and rear-right wheel angular and linear
  speed;
- CARLA-specific longitudinal slip and lateral slip angle for the same wheels.

The external contract is a documented profile of
[COVESA VISS 3.1](https://github.com/COVESA/vehicle-information-service-specification/tree/v3.1)
over a [COVESA VSS 6.0](https://github.com/COVESA/vehicle_signal_specification/tree/v6.0)
signal tree. The initial network profile uses JSON over Secure WebSocket and
supports reading and subscribing to telemetry.

Cameras, LiDAR, radar, and ultrasonic modelling remain deferred. M6 adds a
separate local external-control channel while deliberately keeping VISS
read-only. ROS 2 is not a runtime dependency; a ROS 2 adapter may be added
later for consumers that already use that ecosystem.

## Repository boundaries

This is deliberately separate from both the CARLA source tree and any Unreal
Engine checkout. It must not contain private Epic Games/Unreal Engine code,
binaries, assets, generated data, credentials, or other restricted material.
See [Public repository policy](docs/public-repository-policy.md).

## Build without CARLA

The default build keeps argument parsing and documentation CI independent of a
CARLA installation:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/carla-ego-runtime --version
```

## Build and run with CARLA

Install LibCarla to a local prefix, then enable native connectivity:

```sh
cmake -S . -B build-carla \
  -DCMAKE_BUILD_TYPE=Release \
  -DCARLA_EGO_WITH_CARLA=ON \
  -DCARLA_EGO_WITH_VISS=ON \
  -DCMAKE_PREFIX_PATH="/path/to/carla-install;/path/to/openssl"
cmake --build build-carla
ctest --test-dir build-carla --output-on-failure
./build-carla/carla-ego-runtime --max-frames 20
```

This collects one simulated second at the default fixed step of 0.05 seconds.
Use `--observe-ticks` only when another client is the designated synchronous
tick owner.

To enable M4, create or install a TLS certificate outside the repository and
start the loopback endpoint:

```sh
./build-carla/carla-ego-runtime \
  --max-frames 0 --real-time --autopilot \
  --viss --viss-cert /private/path/server-cert.pem \
  --viss-key /private/path/server-key.pem
```

Then use the separately built client to read a live signal:

```sh
./build-carla/carla-viss-client \
  --host localhost --ca /private/path/trusted-ca.pem \
  --request '{"action":"get","path":"Vehicle.Speed","requestId":"speed-1"}'
```

Or open the continuously updating basic-telemetry dashboard:

```sh
./build-carla/carla-viss-client \
  --host localhost --ca /private/path/trusted-ca.pem --monitor
```

Certificate and key files are ignored by Git and must not be committed.

For a smooth visual M3 demonstration with one physics-driven ego vehicle and
GNSS, run:

```sh
./build-carla/carla-ego-runtime \
  --spawn-point-index 40 \
  --max-frames 0 \
  --real-time \
  --autopilot \
  --chase-camera \
  --chase-camera-response 10 \
  --chase-camera-update-hz 60 \
  --exposure-offset -0.35 \
  --gnss-sensor-tick-seconds 0.1 \
  --gnss-max-age-seconds 0.25 \
  --log-every-frames 20
```

This keeps the existing 20 Hz simulation step but paces it against wall-clock
time. The chase camera smooths one frame-aligned snapshot per physics tick, up
to the configured maximum rate. The exposure offset is restored to zero when
the runtime exits. The vehicle is controlled by CARLA's Traffic Manager rather
than by teleporting it between road positions.

The CARLA server must already be listening on the configured RPC address. See
the complete [native macOS setup](docs/carla-setup-macos.md).

For the repeatable M5 route and restart/endurance acceptance workflow, see
[M5 operations and acceptance](docs/m5-operations.md). The route runner starts
CARLA's official Python BehaviorAgent as the tick owner and the C++ runtime as a
non-owning telemetry observer, then records a manifest and structured log. M5
uses a measured 30 Hz physics cadence while the standalone runtime default
remains 20 Hz.

M5.1 adds an integrated operator launcher. It waits visibly for CARLA, starts
the route, VSS endpoint, and health dashboard as one session, and performs
owned-process cleanup after route completion, failure, or Ctrl-C. Local machine
paths belong in the desktop wrapper or launch command and are not committed to
this public repository.

M6 adds an independent local client that can explicitly acquire and release
vehicle control. The control socket is not VISS: it is a private Unix-domain
channel with a fresh per-run token, and its fail-safe action is zero throttle,
full brake, and centred steering. See
[M6 external-control operations and acceptance](docs/m6-operations.md).

M6.1 adds a macOS-native focused-window keyboard client over that same
control contract. Arrow keys control throttle, brake, and steering; loss of
focus, Space, window close, or client loss selects safe stop. A separate cold-
start launcher keeps the live VSS dashboard visible in Terminal and records a
machine-readable startup timeline. See
[M6.1 keyboard driving](docs/m6-1-keyboard-driving.md).

M6.2 extends the panel and local contract with explicit Manual Control,
Autopilot, and Safe Stop modes. The same ego actor and external-control tick
owner remain active through every handover, while the terminal VSS dashboard
continues to show live telemetry. See
[M6.2 live handover](docs/m6-2-live-handover.md).

For the CARLA-only obstacle and braking-event workflow, see
[deterministic brake-event scenario](docs/brake-event-scenario.md). This path
does not require AosEdge, KUKSA, a vehicle-data provider, or Cloud access.

For reproducible signed Desktop applications that start the accepted M5 and
M6.2 workflows or the deterministic brake event with a visible engineering
dashboard, see [macOS desktop launchers](docs/macos-launchers.md).

## Documentation

- [Native CARLA setup on macOS](docs/carla-setup-macos.md)
- [Architecture](docs/architecture.md)
- [VISS 3.1 compatibility profile](docs/viss-profile.md)
- [VISS/VSS telemetry contract](docs/telemetry-contract.md)
- [Role of ROS 2](docs/ros2-role.md)
- [Roadmap](docs/roadmap.md)
- [M5 operations and acceptance](docs/m5-operations.md)
- [External-control contract](docs/external-control-contract.md)
- [M6 external-control operations and acceptance](docs/m6-operations.md)
- [M6.1 keyboard driving](docs/m6-1-keyboard-driving.md)
- [M6.2 live handover](docs/m6-2-live-handover.md)
- [Deterministic brake-event scenario](docs/brake-event-scenario.md)
- [macOS desktop launchers](docs/macos-launchers.md)
- [Public repository policy](docs/public-repository-policy.md)
- [Architecture decisions](docs/decisions/)
- [Third-party dependencies and licences](THIRD_PARTY.md)

## License

This project is licensed under the [MIT License](LICENSE).
