#!/usr/bin/env python3
"""Install reproducible macOS launchers for the CARLA demo workflows."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import plistlib
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable


REPOSITORY = Path(__file__).resolve().parents[1]
APP_DEFINITIONS = (
    (
        "CARLA Simulator.app",
        "CARLA Simulator",
        "io.github.alexmaninblack.carla-ego-runtime.route",
        "route.command",
    ),
    (
        "CARLA Manual Drive.app",
        "CARLA Manual Drive",
        "io.github.alexmaninblack.carla-ego-runtime.manual-drive",
        "manual-drive.command",
    ),
)


def quote(value: Path | str) -> str:
    return shlex.quote(str(value))


def require_directory(path: Path, label: str) -> Path:
    path = path.expanduser().resolve()
    if not path.is_dir():
        raise ValueError(f"{label} is not a directory: {path}")
    return path


def require_file(
    path: Path,
    label: str,
    executable: bool = False,
    preserve_symlink: bool = False,
) -> Path:
    expanded = path.expanduser()
    path = Path(os.path.abspath(expanded)) if preserve_symlink else expanded.resolve()
    if not path.is_file():
        raise ValueError(f"{label} is not a file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise ValueError(f"{label} is not executable: {path}")
    return path


def require_command(name: str) -> str:
    resolved = shutil.which(name)
    if resolved is None:
        raise ValueError(f"required macOS command is unavailable: {name}")
    return resolved


def atomic_write(path: Path, data: str, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_text(data, encoding="utf-8")
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def route_command(arguments: argparse.Namespace) -> str:
    run_root = arguments.state_directory / "m5" / "runs"
    unreal_log = arguments.state_directory / "m5" / "unreal.log"
    return f"""#!/bin/zsh

set -u

readonly RUNTIME_ROOT={quote(REPOSITORY)}
readonly CARLA_ROOT={quote(arguments.carla_root)}
readonly BUILD_ROOT={quote(arguments.build_root)}
readonly PYTHON={quote(arguments.python)}
readonly TLS_ROOT={quote(arguments.tls_root)}

cd "${{RUNTIME_ROOT}}" || exit 1
"${{PYTHON}}" tools/launch_m5.py \\
  --config config/m5_town10hd_route.json \\
  --runtime "${{BUILD_ROOT}}/carla-ego-runtime" \\
  --viss-client "${{BUILD_ROOT}}/carla-viss-client" \\
  --python "${{PYTHON}}" \\
  --python-api-root "${{CARLA_ROOT}}/PythonAPI/carla" \\
  --certificate "${{TLS_ROOT}}/server-cert.pem" \\
  --private-key "${{TLS_ROOT}}/server-key.pem" \\
  --run-root {quote(run_root)} \\
  --unreal-editor {quote(arguments.unreal_editor)} \\
  --uproject "${{CARLA_ROOT}}/Unreal/CarlaUnreal/CarlaUnreal.uproject" \\
  --startup-map /Game/Carla/Maps/Town10HD_Opt \\
  --unreal-argument=-game \\
  --unreal-argument=-windowed \\
  --unreal-argument=-ResX=1280 \\
  --unreal-argument=-ResY=720 \\
  --unreal-argument=-quality-level=Low \\
  --unreal-argument=-nosound \\
  --unreal-argument=-carla-rpc-port=2000 \\
  --unreal-log {quote(unreal_log)}

readonly STATUS=$?
if [[ ${{STATUS}} -eq 0 ]]; then
  print "\\nRoute finished; all components stopped cleanly."
elif [[ ${{STATUS}} -eq 130 ]]; then
  print "\\nSession stopped by the operator; CARLA cleanup is complete."
else
  print "\\nCARLA route launch failed. Exit code: ${{STATUS}}"
fi
print "You can close this window."
exit ${{STATUS}}
"""


def manual_drive_command(arguments: argparse.Namespace) -> str:
    manual_root = arguments.state_directory / "m6_2"
    keyboard_app = manual_root / "CARLA Keyboard Control.app"
    return f"""#!/bin/zsh

set -u

# Keep the VSS dashboard visible beside the simulator on a typical Mac display.
print -n $'\\e[8;43;72t\\e[3;1140;45t'

readonly RUNTIME_ROOT={quote(REPOSITORY)}
readonly CARLA_ROOT={quote(arguments.carla_root)}
readonly BUILD_ROOT={quote(arguments.build_root)}
readonly PYTHON={quote(arguments.python)}
readonly TLS_ROOT={quote(arguments.tls_root)}

cd "${{RUNTIME_ROOT}}" || exit 1
"${{PYTHON}}" tools/launch_m6_1.py \\
  --config config/m6_2_town10hd_handover.json \\
  --runtime "${{BUILD_ROOT}}/carla-ego-runtime" \\
  --viss-client "${{BUILD_ROOT}}/carla-viss-client" \\
  --python "${{PYTHON}}" \\
  --python-api-root "${{CARLA_ROOT}}/PythonAPI/carla" \\
  --certificate "${{TLS_ROOT}}/server-cert.pem" \\
  --private-key "${{TLS_ROOT}}/server-key.pem" \\
  --keyboard-source "${{RUNTIME_ROOT}}/tools/KeyboardControl.swift" \\
  --keyboard-info "${{RUNTIME_ROOT}}/tools/KeyboardControl-Info.plist" \\
  --keyboard-app {quote(keyboard_app)} \\
  --run-root {quote(manual_root / "runs")} \\
  --unreal-editor {quote(arguments.unreal_editor)} \\
  --uproject "${{CARLA_ROOT}}/Unreal/CarlaUnreal/CarlaUnreal.uproject" \\
  --startup-map /Game/Carla/Maps/Town10HD_Opt \\
  --unreal-argument=-game \\
  --unreal-argument=-windowed \\
  --unreal-argument=-ResX=1100 \\
  --unreal-argument=-ResY=700 \\
  --unreal-argument=-WinX=20 \\
  --unreal-argument=-WinY=70 \\
  --unreal-argument=-quality-level=Low \\
  --unreal-argument=-nosound \\
  --unreal-argument=-carla-rpc-port=2000

readonly STATUS=$?
if [[ ${{STATUS}} -eq 0 ]]; then
  print "\\nLive-handover session finished; CARLA cleanup is complete."
elif [[ ${{STATUS}} -eq 130 ]]; then
  print "\\nSession stopped by the operator; CARLA cleanup is complete."
else
  print "\\nCARLA manual-drive launch failed. Exit code: ${{STATUS}}"
fi
print "You can close this window."
exit ${{STATUS}}
"""


def app_executable(display_name: str, launcher: Path) -> str:
    return f"""#!/bin/zsh

set -u

readonly LAUNCHER={quote(launcher)}

show_error() {{
  local message="$1"
  /usr/bin/osascript - "$message" <<'APPLESCRIPT'
on run argv
  display alert "{display_name} could not start" message (item 1 of argv) as critical buttons {{"OK"}} default button "OK"
end run
APPLESCRIPT
}}

if [[ ! -x "${{LAUNCHER}}" ]]; then
  show_error "The generated launcher is unavailable:\n${{LAUNCHER}}\n\nRun the CARLA launcher installer again."
  exit 1
fi

/usr/bin/open -a Terminal "${{LAUNCHER}}"
exit 0
"""


def build_icon(icon_source: Path, destination: Path) -> None:
    if icon_source.suffix.lower() == ".icns":
        shutil.copy2(icon_source, destination)
        return

    subprocess.run(
        [
            require_command("sips"),
            "-s",
            "format",
            "icns",
            str(icon_source),
            "--out",
            str(destination),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def app_info(display_name: str, bundle_identifier: str) -> dict[str, object]:
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": display_name,
        "CFBundleExecutable": display_name,
        "CFBundleIconFile": "CARLA.icns",
        "CFBundleIdentifier": bundle_identifier,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": display_name,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "0.9.1",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
    }


def build_app(
    app: Path,
    display_name: str,
    bundle_identifier: str,
    launcher: Path,
    icon_source: Path,
) -> None:
    executable = app / "Contents" / "MacOS" / display_name
    resources = app / "Contents" / "Resources"
    resources.mkdir(parents=True)
    atomic_write(executable, app_executable(display_name, launcher), 0o755)
    with (app / "Contents" / "Info.plist").open("wb") as output:
        plistlib.dump(app_info(display_name, bundle_identifier), output, sort_keys=True)
    build_icon(icon_source, resources / "CARLA.icns")
    subprocess.run(
        [require_command("codesign"), "--force", "--deep", "--sign", "-", str(app)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    subprocess.run(
        [require_command("codesign"), "--verify", "--deep", "--strict", str(app)],
        check=True,
    )


def prepare_arguments(arguments: argparse.Namespace) -> argparse.Namespace:
    if sys.platform != "darwin":
        raise ValueError("the desktop launcher installer requires macOS")
    arguments.carla_root = require_directory(arguments.carla_root, "CARLA root")
    arguments.unreal_root = require_directory(arguments.unreal_root, "Unreal Engine root")
    arguments.build_root = require_directory(arguments.build_root, "runtime build root")
    arguments.python = require_file(
        arguments.python,
        "Python interpreter",
        executable=True,
        preserve_symlink=True,
    )
    arguments.tls_root = require_directory(arguments.tls_root, "TLS directory")
    arguments.install_directory = arguments.install_directory.expanduser().resolve()
    arguments.state_directory = arguments.state_directory.expanduser().resolve()
    arguments.unreal_editor = require_file(
        arguments.unreal_root
        / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor",
        "Unreal Editor",
        executable=True,
    )
    require_file(arguments.build_root / "carla-ego-runtime", "CARLA ego runtime", executable=True)
    require_file(arguments.build_root / "carla-viss-client", "CARLA VISS client", executable=True)
    require_directory(arguments.carla_root / "PythonAPI/carla", "CARLA Python API")
    require_file(
        arguments.carla_root / "Unreal/CarlaUnreal/CarlaUnreal.uproject",
        "CARLA Unreal project",
    )
    require_file(arguments.tls_root / "server-cert.pem", "VISS certificate")
    require_file(arguments.tls_root / "server-key.pem", "VISS private key")
    for relative_path, label in (
        ("tools/launch_m5.py", "M5 launcher"),
        ("tools/launch_m6_1.py", "M6.2 launcher"),
        ("tools/KeyboardControl.swift", "keyboard-control source"),
        ("tools/KeyboardControl-Info.plist", "keyboard-control metadata"),
        ("config/m5_town10hd_route.json", "M5 configuration"),
        ("config/m6_2_town10hd_handover.json", "M6.2 configuration"),
    ):
        require_file(REPOSITORY / relative_path, label)
    if arguments.icon_source is None:
        arguments.icon_source = (
            arguments.carla_root
            / "Unreal/CarlaUnreal/Plugins/Carla/Resources/Icon128.png"
        )
    arguments.icon_source = require_file(arguments.icon_source, "CARLA launcher icon")
    return arguments


def install(arguments: argparse.Namespace) -> list[Path]:
    arguments.install_directory.mkdir(parents=True, exist_ok=True)
    arguments.state_directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    arguments.state_directory.chmod(0o700)
    launchers = arguments.state_directory / "launchers"
    route_launcher = launchers / "route.command"
    manual_launcher = launchers / "manual-drive.command"
    atomic_write(route_launcher, route_command(arguments), 0o700)
    atomic_write(manual_launcher, manual_drive_command(arguments), 0o700)
    launcher_paths = {
        "route.command": route_launcher,
        "manual-drive.command": manual_launcher,
    }

    destinations = [arguments.install_directory / definition[0] for definition in APP_DEFINITIONS]
    existing = [path for path in destinations if path.exists() or path.is_symlink()]
    if existing and not arguments.replace:
        names = ", ".join(path.name for path in existing)
        raise ValueError(f"launcher already exists; use --replace to preserve and replace it: {names}")

    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    backup_directory = arguments.state_directory / "backups" / timestamp
    with tempfile.TemporaryDirectory(
        prefix=".carla-launcher-install-", dir=arguments.install_directory
    ) as temporary:
        staging = Path(temporary)
        staged_apps: list[Path] = []
        for app_name, display_name, bundle_identifier, launcher_name in APP_DEFINITIONS:
            app = staging / app_name
            build_app(
                app,
                display_name,
                bundle_identifier,
                launcher_paths[launcher_name],
                arguments.icon_source,
            )
            staged_apps.append(app)

        backed_up: list[tuple[Path, Path]] = []
        installed: list[Path] = []
        try:
            if existing:
                backup_directory.mkdir(parents=True, mode=0o700)
                for destination in existing:
                    backup = backup_directory / destination.name
                    shutil.move(destination, backup)
                    backed_up.append((destination, backup))
            for staged, destination in zip(staged_apps, destinations):
                shutil.move(staged, destination)
                installed.append(destination)
        except Exception:
            for destination in installed:
                if destination.is_dir() and not destination.is_symlink():
                    shutil.rmtree(destination)
                else:
                    destination.unlink(missing_ok=True)
            for destination, backup in reversed(backed_up):
                shutil.move(backup, destination)
            raise

    return destinations


def parse_arguments(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--carla-root", required=True, type=Path)
    parser.add_argument("--unreal-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--python", required=True, type=Path)
    parser.add_argument("--tls-root", required=True, type=Path)
    parser.add_argument("--icon-source", type=Path)
    parser.add_argument(
        "--install-directory", type=Path, default=Path.home() / "Desktop"
    )
    parser.add_argument(
        "--state-directory",
        type=Path,
        default=Path.home() / "Library/Application Support/CARLA Ego Runtime",
    )
    parser.add_argument("--replace", action="store_true")
    return parser.parse_args(argv)


def main() -> int:
    try:
        arguments = prepare_arguments(parse_arguments())
        installed = install(arguments)
        print("Installed CARLA macOS launchers:")
        for path in installed:
            print(f"  {path}")
        print(f"Private generated state: {arguments.state_directory}")
        print("The Manual Drive launcher uses the accepted M6.2 live-handover workflow.")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"CARLA launcher installation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
