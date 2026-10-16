<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: MIT -->

# CARLA Ego Runtime

Vehicle Gateway and native driving/control tooling. It translates simulator data to VISS and owns the local motion-control boundary; QM advisories do not acquire motion authority.

<a id="sdv-lab-entry"></a>

For the **whole demo**, start at the [SDV Lab README](https://github.com/alexmaninblack/aosedge-sdv-demo).
Only the product repository is manually cloned for its pinned multi-component
build. The instructions below are for working on **this component alone**;
a host check does not publish, install or qualify a vehicle package.

## 1 Prepare a macOS component workspace

Use native Apple Silicon Terminal. These component commands are for development,
not a qualified full-demo installation. Run blocks in order and stop on error.

Choose an existing writable local APFS parent folder, on the internal disk or
an external SSD. Use the product launcher instead for automatic full-demo
preparation; this section is only the component workflow:

```sh
uname -m
printf 'Existing local APFS parent folder (absolute path): '
read -r SDV_PARENT
test -d "$SDV_PARENT" && test -w "$SDV_PARENT" && df -h "$SDV_PARENT"
```

Expect `arm64` and enough free space on the selected volume. Stop if the
folder/access check fails; do not create a replacement for a disconnected
volume. An external SSD is optional. After confirming storage:

```sh
SDV_WORK="$SDV_PARENT/sdv-components"
mkdir -p "$SDV_WORK"
export HOMEBREW_CACHE="$SDV_WORK/cache/homebrew"
```

Install Apple's Command Line Tools with `xcode-select --install` if missing,
and finish the system dialog. Install [Homebrew](https://docs.brew.sh/Installation)
if absent. Then:

```sh
eval "$(/opt/homebrew/bin/brew shellenv)"
brew install cmake python@3.12
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
git --version
cmake --version
python3 --version
xcrun clang++ --version
```

Do not use the installed demo's private interpreter or a Rosetta toolchain.

## 2 Clone this component

```sh
git clone --branch main https://github.com/alexmaninblack/carla-ego-runtime.git "$SDV_WORK/carla-ego-runtime"
cd "$SDV_WORK/carla-ego-runtime"
git rev-parse HEAD
```

Record the printed revision with your results. `main` is current development,
not a release pin. To reproduce the complete candidate, use the product
repository's manifest-driven route instead of independently choosing branches.

## 3 Build and check host targets

```sh
SDV_BUILD="$SDV_WORK/build/carla-ego-runtime"
cmake -S . -B "$SDV_BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DCARLA_EGO_WITH_CARLA=OFF -DCARLA_EGO_WITH_VISS=OFF
cmake --build "$SDV_BUILD" --parallel 2
ctest --test-dir "$SDV_BUILD" --output-on-failure
```

Expect a successful build and no failed CTest cases. Keep the first failure;
do not replace expected results or lower resource/security requirements.

This route requires CMake 3.24 or later and a C++20 compiler. It deliberately
has **no CARLA connection or VISS server**. Keep Python available when
configuring so the optional Python CTest cases are registered.

## 4 Check the executable, or choose the live route

```sh
"$SDV_BUILD/carla-ego-runtime" --version
"$SDV_BUILD/carla-ego-runtime" --help
```

Expected: version/usage output and normal exit, not a simulator window.
For the real Gateway use the product repository's developer chain, which
supplies the pinned LibCarla SDK and native TLS dependencies. For specialist
component-only work see [native macOS setup](docs/carla-setup-macos.md).
Do not start that standalone launcher over a Demo Control-owned simulator.

The installed demo reuses **standalone CARLA without Unreal Editor**. Opening
an Editor or compiling Unreal is not a prerequisite for checking this component.

## 5 Finish

The version/help and CTest commands exit on completion; no simulator was
started. Live standalone launchers are stopped through their foreground owner
(Ctrl-C where documented). Integrated sessions are stopped through Demo Control.
Do not use a broad process kill or stop unrelated Docker workloads.

## Component documentation

- [Architecture](docs/architecture.md)
- [VISS profile](docs/viss-profile.md)
- [External-control contract](docs/external-control-contract.md)
- [Native macOS setup](docs/carla-setup-macos.md)
- [License](LICENSE) and [third-party dependencies](THIRD_PARTY.md)

## Runtime reference

CARLA supplies the simulated vehicle. This component owns its control bridge,
VSS projection and TLS VISS endpoint; AosEdge services run in the separate
ARM64 controller. The installed lab uses standalone CARLA, not Unreal Editor.

- [Telemetry contract](docs/telemetry-contract.md)
- [Driving maneuvers and brake-event scenario](docs/brake-event-scenario.md)
- [Replaceable control source](docs/external-control-contract.md)
- [Standalone component fixtures and wrappers](docs/standalone-fixtures.md)
- [Architecture decisions](docs/decisions/)
- [Third-party dependencies](THIRD_PARTY.md)

Use Demo Control for an installed lab; standalone development launchers must
not run over its simulator/controller owners. Return to road ends stationary
in Manual and does not reset service models or start Autopilot.

## License

Original work is [MIT-licensed](LICENSE). Third-party components retain their
own terms. Keep local credentials, run data and generated build outputs out of Git.
