# Native CARLA setup on macOS

## Tested baseline

M1 is developed and tested against:

- CARLA fork: `alexmaninblack/carla`;
- branch: `macos-apple-silicon`;
- commit:
  [`6296236e1abd205aa8efb6b5991dfef34e95c33a`](https://github.com/alexmaninblack/carla/commit/6296236e1abd205aa8efb6b5991dfef34e95c33a);
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

## Start CARLA and validate M1

Start the CARLA Unreal application, or open `CarlaUnreal.uproject` in Unreal
Editor and enter Play mode. The server must listen on RPC port 2000 before the
runtime is started.

Run a ten-second lifecycle check:

```sh
./build-carla/carla-ego-runtime --run-seconds 10
```

Expected output includes:

- the LibCarla and server versions;
- the current map name;
- whether an existing `hero` vehicle was selected or a new one was spawned;
- the actor ID and blueprint type;
- destruction of the actor on exit only when this runtime spawned it.

Use `--no-spawn` to make the check read-only. Use Ctrl-C to stop a bounded run
early; a runtime-owned actor is still cleaned up.

The M1 acceptance test on the baseline above used matching client/server
version `0.10.0` and `Town10HD_Opt`. One runtime spawned `hero`, two independent
`--no-spawn` clients selected the same actor without deleting it, Ctrl-C on the
owning runtime destroyed it, and a final `--no-spawn` check confirmed its
absence. A second bounded spawn-and-cleanup cycle also passed.

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
| `--run-seconds` | `0` | Keep the M1 connection alive for a bounded test |

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
