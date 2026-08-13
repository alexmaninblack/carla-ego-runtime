# macOS desktop launchers

`tools/install_macos_launchers.py` creates two signed application bundles:

- `CARLA Simulator.app` starts the repeatable M5 route and VISS dashboard;
- `CARLA Manual Drive.app` starts the accepted M6.2 manual/autopilot handover
  workflow and VISS dashboard.

The bundles contain only a generic wrapper and a CARLA icon. Machine-specific
paths and generated launch commands are written outside the repository. TLS
private keys, runtime binaries, Unreal Engine files, and CARLA assets are never
copied into this public repository or into the application bundles.

## Install

Run the installer from the repository checkout and provide the local paths:

```sh
./tools/install_macos_launchers.py \
  --carla-root /path/to/CarlaSim \
  --unreal-root /path/to/UnrealEngine5_carla \
  --build-root /path/to/CarlaSim/Build-ego-runtime-m4 \
  --python /path/to/CarlaSim/.venv-m5/bin/python \
  --tls-root /private/path/to/viss-tls
```

By default, the applications are installed on the current user's Desktop and
private generated state is stored in:

```text
~/Library/Application Support/CARLA Ego Runtime
```

The generated state directory is restricted to the current user. It contains
launch commands, logs, run artifacts, and the compiled keyboard-control app.
It refers to the TLS directory supplied to the installer but does not copy the
certificate or private key.

## Replace an existing installation

The installer refuses to overwrite an existing application unless `--replace`
is present:

```sh
./tools/install_macos_launchers.py [same path options] --replace
```

Before replacement, existing application bundles are moved to a timestamped
directory below `CARLA Ego Runtime/backups`. If building or installing either
new bundle fails, the previous bundles are restored.

Re-run the installer after moving a source checkout, build directory, Python
environment, or TLS directory. No simulator is launched during installation.
