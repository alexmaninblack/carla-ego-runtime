# CARLA Ego Runtime

A native C++ runtime for one instrumented ego vehicle in the
[CARLA simulator](https://github.com/carla-simulator/carla). It will expose
vehicle telemetry through the COVESA Vehicle Information Service Specification
(VISS).

> [!IMPORTANT]
> This repository currently contains a buildable project scaffold and design
> documentation only. It does not connect to CARLA or serve telemetry yet.

## Initial scope

The first runtime milestone intentionally uses only low-cost vehicle state and
GNSS:

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
are out of scope for the first milestone. ROS 2 is not a runtime dependency; a
ROS 2 adapter may be added later for consumers that already use that ecosystem.

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
- [VISS 3.1 compatibility profile](docs/viss-profile.md)
- [VISS/VSS telemetry contract](docs/telemetry-contract.md)
- [Role of ROS 2](docs/ros2-role.md)
- [Roadmap](docs/roadmap.md)
- [Public repository policy](docs/public-repository-policy.md)
- [Architecture decisions](docs/decisions/)

## License

This project is licensed under the [MIT License](LICENSE).
