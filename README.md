# CARLA Ego Runtime

A native C++ runtime and future network gateway for one instrumented ego
vehicle in the [CARLA simulator](https://github.com/carla-simulator/carla).

> [!IMPORTANT]
> This repository currently contains a buildable project scaffold and design
> documentation only. It does not connect to CARLA or publish telemetry yet.

## Initial scope

The first milestone intentionally uses only low-cost vehicle state and GNSS:

- signed vehicle speed;
- throttle and brake commands;
- normalized steering command;
- actual front-wheel steering angles;
- world and longitudinal acceleration;
- GNSS latitude, longitude, and altitude;
- simulation frame and timestamp on every message.

Cameras, LiDAR, radar, ultrasonic modelling, ROS 2, and external autonomous
control are out of scope for the first milestone.

## Repository boundaries

This is deliberately separate from both the CARLA source tree and any Unreal
Engine checkout. It must not contain private Epic Games/Unreal Engine code,
binaries, assets, generated data, credentials, or other restricted material.
See [Public repository policy](docs/public-repository-policy.md).

## Build the scaffold

A CARLA installation is not required until connectivity is implemented.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/carla-ego-runtime --version
```

## Documentation

- [Architecture](docs/architecture.md)
- [Draft telemetry contract](docs/telemetry-contract.md)
- [Roadmap](docs/roadmap.md)
- [Public repository policy](docs/public-repository-policy.md)
- [Architecture decisions](docs/decisions/)

## License

This project is licensed under the [MIT License](LICENSE).
