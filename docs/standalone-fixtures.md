<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# Standalone Gateway fixtures and desktop wrappers

These are optional **component-development** tools, not the installed SDV Lab
startup instructions. For the whole lab use
[Setup and Demo Control](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/getting-started/installed-preview-cloud-first-use.md).
Never run a second fixture/launcher over its existing simulator or tick owner.
The installed lab already supplies standalone CARLA without Unreal Editor.

## Choose the fixture by purpose

| Purpose | Existing entry / configuration | Boundary |
| --- | --- | --- |
| Routed BehaviorAgent drive | `tools/run_m5.py`, `config/m5_town10hd_route.json` | BehaviorAgent owns actor and synchronous clock; exact fixture pins apply |
| Bounded external-control acceptance | `tools/run_m6.py`, `config/m6_town10hd_external_control.json` | Independent client exercises private command ownership, deadlines and disconnect |
| Focused manual-only panel | `tools/launch_m6_1.py` | Legacy compatible manual fixture, not the integrated handover UI |
| Manual/autopilot handover | `tools/launch_m6_1.py` with `config/m6_2_town10hd_handover.json` | One actor/tick owner; native panel switches Manual, Autopilot and Safe Stop |
| Repeatable obstacle braking | [Brake-event guide](brake-event-scenario.md) | Hybrid interactive and bounded unattended profiles remain separate |

Tool/configuration names retain their compatibility identifiers. Their numbers
are not stages a new user must repeat. The current
[external-control](external-control-contract.md), [telemetry](telemetry-contract.md)
and [VISS](viss-profile.md) contracts own semantics; this guide does not copy a
second wire specification.

## Inputs and component execution

Build instructions are in [native macOS setup](carla-setup-macos.md). Supply the
matching runtime, independent VISS client, Python/BehaviorAgent environment,
CARLA Python API and private server TLS inputs. The routed fixture pins its
own simulator revision/map/spawn/timing; it must reject incompatible inputs,
not be silently repointed at the newest SDK. Its Python dependencies are
`tools/requirements-m5.txt`; generated binaries/environments stay outside Git.

For the bounded control fixture, after starting the matching CARLA server:

```sh
python3 tools/run_m6.py \
  --config config/m6_town10hd_external_control.json \
  --runtime /path/to/owned/build/carla-ego-runtime \
  --viss-client /path/to/owned/build/carla-viss-client \
  --python /path/to/matching/carla-python \
  --python-api-root /path/to/CARLA/PythonAPI/carla \
  --certificate /private/path/server-cert.pem \
  --private-key /private/path/server-key.pem \
  --run-root /private/path/control-fixture-runs
```

The routed fixture uses the same common inputs with `tools/run_m5.py` and its
route configuration. Inspect each entry's `--help` for its exact options before
running. `--repeat` requests complete sequential sessions; `--route-cycles`
extends the route only in the routed fixture and records its effective timing.
Headless/dashboard options change observation rendering, not simulator physics
or proof requirements. Do not infer hidden rendering behavior from a flag name.

Some standalone wrappers can start an explicitly configured Editor process when
their CARLA server is absent. That specialist capability is not an installed-lab
prerequisite. Local paths, keys, artifacts and proprietary engine files remain
outside public source. Do not replace loopback fixtures with LAN endpoints or
represent server-only fixture TLS as selected-Unit mTLS.

## Native controls and ownership

The handover panel selects Manual (M/Enter), Autopilot (A) or Safe Stop (Space).
Arrow keys provide normalized manual controls. Escape/panel close ends the
standalone session. Focus loss stops Manual, not intentionally selected
Autopilot. Ownership expiry, disconnect and explicit shutdown stop motion;
manual-command and heartbeat deadlines are defined by the control contract.

Actor, camera, GNSS, telemetry and synchronous tick owner stay the same during
mode changes. Traffic Manager drives that same actor only in Autopilot; no
second BehaviorAgent runs beside it. Scenario-capable profiles additionally
advertise Start/Restart Scripted Scenario (S); Manual aborts only that attempt,
and completion leaves the persistent session stopped for further interaction.
Requested mode is never reported as applied before a completed physical frame.

## Optional desktop wrappers

`tools/install_macos_launchers.py` creates standalone `CARLA Simulator.app`,
`CARLA Manual Drive.app` and `CARLA Brake Event.app`. They are **not** the signed
SDV Lab installer or another way to open an installed instance. Inspect the
component installer's help, then supply explicit local paths:

```sh
./tools/install_macos_launchers.py \
  --carla-root /path/to/CarlaSim \
  --unreal-root /path/to/UnrealEngine5_carla \
  --build-root /path/to/owned/gateway-build \
  --python /path/to/matching/carla-python \
  --tls-root /private/path/viss-tls
```

By default bundles go to the current Desktop and private generated state to
`~/Library/Application Support/CARLA Ego Runtime`. They contain generic wrappers
and permitted artwork, not runtime dependencies or TLS keys. Installation
starts no simulator. `--replace` preserves the existing bundle under the
tool's owned backup transaction and restores it if replacement fails.
Run the installer again after deliberately moving its referenced inputs.

The standalone manual/brake wrappers own their complete visible session.
They may close a warm CARLA only after verifying executable/project/RPC-port
identity. Direct low-level launch remains conservative unless the exact
`--close-reused-simulator-on-exit` behavior was requested. This does not change
Demo Control's installed Test stop/retention/deletion rules.

## Evidence and cleanup

Private manifests, process events, controller status and startup timelines
describe the fixture's exact configuration, real motion, VISS probes, owned
actors and result. A route PASS requires its complete legs and measured native
data, not a timer. The bounded control client separately checks command expiry,
recovery, release and intentional disconnect; loss selects zero throttle, full
brake and centered steering. A window opening is not any of those proofs.

Use orderly owner shutdown. Stop runtime/GNSS before destroying the owned ego,
restore original world settings and remove private socket/token resources.
Remove only actors/processes this fixture owns; never a broad kill or shared
Docker shutdown. Keep bounded failure evidence without keys/raw credentials.
Actual process/listener cleanup is part of completion.

These fixtures retain valuable unit/component coverage but do not establish
integrated publication, provisioning, service products, full UI or installer
acceptance. Exact media and M1 proof belong to the integration baseline. Earlier
milestone-specific repetition and measurements remain in
[Git history](https://github.com/alexmaninblack/carla-ego-runtime/tree/4da65485cf0614dd3ed61c813318b9ba5ba07b38/docs).
