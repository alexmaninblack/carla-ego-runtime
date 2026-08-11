# CARLA Ego Runtime

A native C++ runtime for one instrumented ego vehicle in the
[CARLA simulator](https://github.com/carla-simulator/carla). It will expose
vehicle telemetry through the COVESA Vehicle Information Service Specification
(VISS).

> [!IMPORTANT]
> M2 basic vehicle-state collection is implemented. The runtime owns a
> synchronous 20 Hz simulation clock by default, publishes exactly one
> timestamped VSS snapshot per CARLA frame, and retains only the latest
> snapshot. GNSS and the network-facing VISS server remain later milestones.

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
- frame-aligned speed, acceleration, pedal, steering, gear, and RPM sampling;
- transport-independent normalization and VSS 6.0 projection;
- a bounded latest-value signal store and project-owned simulation overlay;
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
  -DCMAKE_PREFIX_PATH=/path/to/carla-install
cmake --build build-carla
ctest --test-dir build-carla --output-on-failure
./build-carla/carla-ego-runtime --max-frames 20
```

This collects one simulated second at the default fixed step of 0.05 seconds.
Use `--observe-ticks` only when another client is the designated synchronous
tick owner.

The CARLA server must already be listening on the configured RPC address. See
the complete [native macOS setup and M2 runbook](docs/carla-setup-macos.md).

## Documentation

- [Native CARLA setup on macOS](docs/carla-setup-macos.md)
- [Architecture](docs/architecture.md)
- [VISS 3.1 compatibility profile](docs/viss-profile.md)
- [VISS/VSS telemetry contract](docs/telemetry-contract.md)
- [Role of ROS 2](docs/ros2-role.md)
- [Roadmap](docs/roadmap.md)
- [Public repository policy](docs/public-repository-policy.md)
- [Architecture decisions](docs/decisions/)

## License

This project is licensed under the [MIT License](LICENSE).
