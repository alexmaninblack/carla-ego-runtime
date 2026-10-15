<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# Native Gateway development on macOS

For the installed lab, use the [Setup guide](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/getting-started/installed-preview-cloud-first-use.md).
It uses standalone CARLA without Unreal Editor. Do not run a standalone
Gateway or development launcher over a Demo Control-owned simulator.

For a host-only component check without CARLA/VISS, follow the
[component README](../README.md). The native path below is for contributors
who already have the exact compatible simulator, LibCarla SDK and TLS inputs.

## Obtain matching dependencies

The normal [developer route](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/getting-started/reproduce-demo.md)
prepares the reviewed binary inputs and compiles Gateway against them.
The [simulator dependency lock](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/workspace/dependencies/carla-macos-arm64-r1.lock.json)
and selected producer plan own source correspondence, inventories and archive
digests. Use those inputs; an old milestone branch or the newest arbitrary
LibCarla checkout is not a substitute.

LibCarla must provide the relocatable `Carla::carla-client` CMake package,
matching public headers/static libraries and notices. Native connectivity
requires C++20. OpenSSL supplies the TLS dependency. For a specialist rebuild
of the SDK/simulator itself, use the
[controller/simulator source scope](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/getting-started/full-source-build.md).
The normal Gateway build does not compile Unreal or recook maps.

## Build the native component

From this repository, substitute the verified dependency prefixes and a
dedicated output directory:

```sh
cmake -S . -B /path/to/owned/gateway-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCARLA_EGO_WITH_CARLA=ON -DCARLA_EGO_WITH_VISS=ON \
  -DCMAKE_PREFIX_PATH="/path/to/verified/carla-install;/path/to/verified/openssl"
cmake --build /path/to/owned/gateway-build --parallel 2
ctest --test-dir /path/to/owned/gateway-build --output-on-failure
/path/to/owned/gateway-build/carla-ego-runtime --version
/path/to/owned/gateway-build/carla-ego-runtime --help
```

These are contributor build commands, not commands required for installing
the demo. Configuration fails on missing/incompatible dependencies. Preserve
actual test failures and never change expected digests to adopt unknown inputs.
Version/help output exits normally; it does not start a simulator.

## Live component sessions

Use Demo Control for the integrated lab. It owns CARLA, Driving Control,
source attachment, selected-Unit mTLS, startup ordering and final shutdown.

Independent standalone fixtures remain available for their explicit purposes:

- [Routed drive](m5-operations.md): BehaviorAgent owns the actor and clock.
- [External-control acceptance](m6-operations.md): bounded private control-channel checks.
- [Manual-only panel](m6-1-keyboard-driving.md): focused standalone manual fixture.
- [Manual/autopilot handover](m6-2-live-handover.md): one actor, one tick owner.
- [Brake-event exercise](brake-event-scenario.md): deterministic physical maneuver.
- [Standalone desktop wrappers](macos-launchers.md): optional source-development launchers.

Supply matching local runtime/Python/configuration and protected TLS material.
Standalone profiles use their declared identity and trust configuration; a
loopback development certificate is not integrated selected-Unit mTLS.
Do not expose a development endpoint to the LAN, disable verification, use a
Provider credential as a service token or invent Unit identity.

## Runtime behavior and cleanup

Gateway samples every accepted simulation frame, retains a bounded latest
snapshot, preserves source timestamps and owns only actors/sensors it created.
The [telemetry contract](telemetry-contract.md) and [VISS profile](viss-profile.md)
define units, paths, availability, strict roles and typed advisory. General
telemetry writes remain rejected; the narrow Brake/Tire advisory return does
not grant motion authority. Vehicle commands use the separate
[local control channel](external-control-contract.md).

The configured owner restores world settings and removes owned GNSS before
the owned vehicle. Observer clients must not destroy an existing hero.
Use the selected launcher's orderly stop, retain its result and independently
check process/listener cleanup. Do not use broad process kills or stop shared
Docker. Source checks, standalone fixture passes and version/help output do
not qualify a new integrated installer; exact results belong to the
[current baseline](https://github.com/alexmaninblack/aosedge-sdv-demo/blob/main/docs/qualification/current-baseline.md).
