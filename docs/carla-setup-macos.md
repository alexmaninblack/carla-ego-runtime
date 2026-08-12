# Native CARLA setup on macOS

## Tested baseline

M1–M5 are developed and tested against:

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

M5 additionally builds the matching arm64 CARLA Python wheel from this same
commit for the external BehaviorAgent control source. The wheel and its local
virtual environment remain outside the public runtime repository. See
[M5 operations and acceptance](m5-operations.md).

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
  -DCARLA_EGO_WITH_VISS=ON \
  -DCMAKE_PREFIX_PATH="/path/to/carla-install;/path/to/openssl"
cmake --build build-carla
ctest --test-dir build-carla --output-on-failure
```

LibCarla requires C++20, so enabling CARLA connectivity raises the complete
runtime build to C++20. The default dependency-free build uses the same
language level to prevent the two modes from drifting.

## Start CARLA and validate M3

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
- creation of a 10 Hz GNSS sensor and a source `gnss_frame` in VSS summaries;
- accepted/rejected GNSS fix counters and sensor cleanup before vehicle cleanup;
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

The M3 acceptance test used a 12-second real-time drive on `Town10HD_Opt`.
The runtime published 240 vehicle-state VSS snapshots, accepted exactly 120
10 Hz GNSS fixes, rejected none, projected 19 points when all optional values
were available, and destroyed GNSS actor `36` before ego vehicle `35`.

## Enable and validate M4

The VISS build needs OpenSSL 3 and uses the Boost.Beast, Asio, JSON, and System
headers already distributed in the pinned LibCarla install. On Homebrew macOS,
`/opt/homebrew/opt/openssl@3` is a suitable second CMake prefix.

Use a trusted certificate for any persistent deployment. For a short loopback
development test only, a self-signed certificate may be generated outside the
repository:

```sh
mkdir -p /private/tmp/carla-viss-tls
openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
  -keyout /private/tmp/carla-viss-tls/server-key.pem \
  -out /private/tmp/carla-viss-tls/server-cert.pem \
  -days 1 -subj /CN=localhost \
  -addext subjectAltName=DNS:localhost,IP:127.0.0.1
```

Start a live loopback service:

```sh
./build-carla/carla-ego-runtime \
  --max-frames 0 --real-time --autopilot --log-every-frames 100 \
  --viss \
  --viss-cert /private/tmp/carla-viss-tls/server-cert.pem \
  --viss-key /private/tmp/carla-viss-tls/server-key.pem
```

Read speed from a second terminal with certificate and host-name verification:

```sh
./build-carla/carla-viss-client \
  --host localhost --port 6443 \
  --ca /private/tmp/carla-viss-tls/server-cert.pem \
  --request '{"action":"get","path":"Vehicle.Speed","requestId":"speed-1"}'
```

Read a subscription acknowledgement and two events:

```sh
./build-carla/carla-viss-client \
  --host localhost --port 6443 \
  --ca /private/tmp/carla-viss-tls/server-cert.pem --messages 3 \
  --request '{"action":"subscribe","path":"Vehicle","filter":[{"variant":"paths","parameter":["Speed","Chassis.Brake.PedalPosition","CurrentLocation.*"]},{"variant":"timebased","parameter":{"period":"100"}}],"requestId":"telemetry-1"}'
```

The local M4 acceptance run on `Town10HD_Opt` used matching CARLA/LibCarla
`0.10.0`. The runtime published 900 state frames and accepted 450 GNSS fixes
with no rejections. An independent verified-TLS client read live speed, all
three `Vehicle.CurrentLocation.*` points, and consecutive 100 ms subscription
events. Server metrics reported two accepted clients, two events, zero dropped
events, and zero coalesced intervals. The automated network test additionally
verified malformed requests, Update rejection, reconnect isolation, TLS
version, and rejection of a non-`VISSv3` handshake.

Do not bind outside `127.0.0.1` with the development certificate. Testing from
a second computer requires a certificate trusted for the server's network name,
an explicit `--viss-bind-address`, firewall configuration, and the threat review
listed in the VISS profile.

## Validate M5

Build and install the matching arm64 Python API wheel, then use the checked-in
M5 runner and fixed route as described in
[M5 operations and acceptance](m5-operations.md). The local Apple Silicon
acceptance completed the 767.63 m route visually and across two consecutive
restart runs. Both independent VISS probes passed in both runs, all processes
exited cleanly, world settings were restored, and no owned vehicle, GNSS actor,
or VISS listener remained afterward.

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
| `--real-time` | off | Pace owned ticks against wall-clock time |
| `--autopilot` | off | Enable synchronous Traffic Manager control |
| `--chase-camera` | off | Follow the ego vehicle with the spectator |
| `--chase-camera-response` | `10` | Camera smoothing response; lower values are smoother but lag more |
| `--chase-camera-update-hz` | `60` | Independent chase-camera interpolation rate |
| `--exposure-offset` | `0` | Temporary Unreal exposure compensation in EV, restored on exit |
| `--gnss-sensor-tick-seconds` | `0.1` | GNSS measurement period |
| `--gnss-max-age-seconds` | `0.25` | Omit older retained GNSS fixes |
| `--log-every-frames` | `1` | Print one sample summary every N frames |
| `--viss` | off | Enable the TLS-only VISS endpoint |
| `--viss-bind-address` | `127.0.0.1` | VISS listener address |
| `--viss-port` | `6443` | VISS Secure WebSocket port |
| `--viss-cert` | none | PEM certificate chain, required with `--viss` |
| `--viss-key` | none | PEM private key, required with `--viss` |
| `--viss-max-clients` | `8` | Concurrent VISS client cap |
| `--viss-max-subscriptions` | `16` | Subscription cap per client |
| `--viss-max-pending-messages` | `8` | Outbound queue cap per client |
| `--observe-ticks` | off | Wait for an external tick owner without changing world settings |

For a visual live view of the VSS basic telemetry, run the bundled client with
`--monitor` instead of `--request`. It creates a time-based VISS subscription
and refreshes a compact terminal dashboard until Ctrl-C.

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
- **VISS certificate error** — provide readable PEM certificate and private-key
  files whose key pair matches; do not add them to the repository.
- **VISS host verification error** — connect with a host name present in the
  certificate SAN and use the CA that issued that certificate.
