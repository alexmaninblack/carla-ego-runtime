# Native CARLA setup on macOS

## Tested baseline

M1/M2 are developed and tested against:

- CARLA fork: `alexmaninblack/carla`;
- branch: `macos-apple-silicon`;
- commit:
  [`385927b6ac5efaaa204b5b9853a7aaa5c5917428`](https://github.com/alexmaninblack/carla/commit/385927b6ac5efaaa204b5b9853a7aaa5c5917428);
- architecture: Apple Silicon (`arm64`);
- compiler: AppleClang with C++20;
- default CARLA RPC endpoint: `127.0.0.1:2000`.

The pinned CARLA commit installs a relocatable `Carla::carla-client` CMake
package together with the exact public dependency headers, static libraries,
and third-party licence files used by LibCarla. The runtime never includes a
CARLA source or build directory directly.

## Install LibCarla

From a clean checkout of the pinned CARLA commit:

```sh
cmake -S /path/to/carla -B /path/to/carla/Build-client \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CARLA_CLIENT=ON \
  -DBUILD_CARLA_UNREAL=OFF \
  -DBUILD_PYTHON_API=OFF
cmake --build /path/to/carla/Build-client --target carla-client
cmake --install /path/to/carla/Build-client \
  --prefix /path/to/carla-install
```

The install prefix contains `include/`, `lib/`, licence notices, and
`lib/cmake/Carla/CarlaConfig.cmake`. It is a local build prerequisite and must
not be committed to this repository.

## Build the runtime

```sh
cmake -S . -B build-carla \
  -DCMAKE_BUILD_TYPE=Release \
  -DCARLA_EGO_WITH_CARLA=ON \
  -DCMAKE_PREFIX_PATH=/path/to/carla-install
cmake --build build-carla
ctest --test-dir build-carla --output-on-failure
```

LibCarla requires C++20, so enabling CARLA connectivity raises the complete
runtime build to C++20. The default dependency-free build uses the same
language level to prevent the two modes from drifting.

## Start CARLA and validate M2

Start the CARLA Unreal application, or open `CarlaUnreal.uproject` in Unreal
Editor and enter Play mode. The server must listen on RPC port 2000 before the
runtime is started.

Collect 20 frame-aligned VSS snapshots (one simulated second at 20 Hz):

```sh
./build-carla/carla-ego-runtime --max-frames 20
```

Expected output includes:

- the LibCarla and server versions;
- the current map name;
- whether an existing `hero` vehicle was selected or a new one was spawned;
- the actor ID and blueprint type;
- synchronous tick ownership and the fixed step;
- frame ID, simulation time, UTC timestamp, speed, and VSS point count for
  every accepted frame;
- confirmation that only one latest snapshot was retained;
- destruction of the actor on exit only when this runtime spawned it.

Use `--no-spawn` to avoid creating an actor. This does not make the default run
fully read-only because the tick owner temporarily enables synchronous mode.
Use `--observe-ticks` for a world-settings read-only client when another client
already owns and advances the simulation clock. Ctrl-C still restores settings
and cleans up a runtime-owned actor.

The M1 acceptance test on the baseline above used matching client/server
version `0.10.0` and `Town10HD_Opt`. One runtime spawned `hero`, two independent
`--no-spawn` clients selected the same actor without deleting it, Ctrl-C on the
owning runtime destroyed it, and a final `--no-spawn` check confirmed its
absence. A second bounded spawn-and-cleanup cycle also passed.

The M2 acceptance test used the same `0.10.0` client/server pair on
`Town10HD_Opt`. Frames 257–259 were sampled with simulation times 7.949,
7.999, and 8.049 seconds. Each produced 14 co-timestamped VSS points, the store
reported three accepted updates with one retained snapshot, and the owned ego
vehicle was destroyed. The pinned CARLA commit replaces the UE5 zero-value
wheel-angle stub with live Chaos wheel state.

## Command-line options

| Option | Default | Meaning |
| --- | --- | --- |
| `--host` | `127.0.0.1` | CARLA RPC host |
| `--port` | `2000` | CARLA RPC port |
| `--timeout-ms` | `10000` | Per-operation network timeout |
| `--role-name` | `hero` | Ego vehicle role to select or assign |
| `--blueprint` | `vehicle.lincoln.mkz` | Vehicle spawned when the role is absent |
| `--spawn-point-index` | `0` | First recommended spawn point to try |
| `--no-spawn` | off | Fail instead of creating an ego vehicle |
| `--allow-version-mismatch` | off | Continue after a client/server mismatch warning |
| `--max-frames` | `1` | Number of snapshots; `0` is unlimited |
| `--run-seconds` | `0` | Optional wall-clock limit; when positive it disables the implicit one-frame limit unless `--max-frames` is also supplied |
| `--fixed-delta-seconds` | `0.05` | Simulation step used by the tick owner |
| `--observe-ticks` | off | Wait for an external tick owner without changing world settings |

The runtime tries every recommended map spawn point in deterministic order if
the configured first point is occupied. It fails without changing the world if
the blueprint is absent or every spawn point is blocked.

## Troubleshooting

- **Connection refused** — Unreal Editor is not in Play mode, the standalone
  simulator is not running, or the configured port is wrong.
- **Connection timed out** — the process is reachable but not answering within
  `--timeout-ms`; check editor startup, map loading, and firewall state.
- **Version mismatch** — rebuild and reinstall LibCarla from the same commit as
  the running CARLA server. Bypass only for an intentional compatibility test.
- **No spawn points available** — choose a map with recommended spawn points or
  start with an existing vehicle carrying the requested `role_name`.
- **Observer times out** — no other client is advancing the synchronous world;
  start the designated owner or omit `--observe-ticks`.
