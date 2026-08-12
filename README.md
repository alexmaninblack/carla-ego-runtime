# CARLA Ego Runtime

A native C++ runtime for one instrumented ego vehicle in the
[CARLA simulator](https://github.com/carla-simulator/carla). It will expose
vehicle telemetry through the COVESA Vehicle Information Service Specification
(VISS).

> [!IMPORTANT]
> The local M4 path is implemented. The runtime owns a synchronous 20 Hz
> simulation clock, retains one timestamped VSS snapshot, and exposes it through
> a TLS-only VISS 3.1 WebSocket endpoint. Binding outside loopback remains an
> explicit deployment and security step.

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
- frame-aligned speed, acceleration, pedal, steering, gear, and RPM sampling;
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
- a native build mode against an installed `Carla::carla-client` package.

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
- simulation run, frame, and time metadata.

The external contract is a documented profile of
[COVESA VISS 3.1](https://github.com/COVESA/vehicle-information-service-specification/tree/v3.1)
over a [COVESA VSS 6.0](https://github.com/COVESA/vehicle_signal_specification/tree/v6.0)
signal tree. The initial network profile uses JSON over Secure WebSocket and
supports reading and subscribing to telemetry.

Cameras, LiDAR, radar, ultrasonic modelling, and external autonomous control
are out of scope for the first telemetry milestone. ROS 2 is not a runtime
dependency; a ROS 2 adapter may be added later for consumers that already use
that ecosystem.

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
time. The chase camera interpolates independently at 60 Hz, and the exposure
offset is restored to zero when the runtime exits. The vehicle is controlled by
CARLA's Traffic Manager rather than by teleporting it between road positions.

The CARLA server must already be listening on the configured RPC address. See
the complete [native macOS setup and M4 runbook](docs/carla-setup-macos.md).

## Documentation

- [Native CARLA setup on macOS](docs/carla-setup-macos.md)
- [Architecture](docs/architecture.md)
- [VISS 3.1 compatibility profile](docs/viss-profile.md)
- [VISS/VSS telemetry contract](docs/telemetry-contract.md)
- [Role of ROS 2](docs/ros2-role.md)
- [Roadmap](docs/roadmap.md)
- [Public repository policy](docs/public-repository-policy.md)
- [Architecture decisions](docs/decisions/)
- [Third-party dependencies and licences](THIRD_PARTY.md)

## License

This project is licensed under the [MIT License](LICENSE).
