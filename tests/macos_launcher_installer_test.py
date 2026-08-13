import plistlib
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
INSTALLER = REPOSITORY / "tools" / "install_macos_launchers.py"
@unittest.skipUnless(sys.platform == "darwin", "macOS launcher test")
class MacOSLauncherInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.carla = self.root / "CarlaSim"
        self.unreal = self.root / "UnrealEngine"
        self.build = self.root / "build"
        self.tls = self.root / "tls"
        self.desktop = self.root / "Desktop"
        self.state = self.root / "state"
        self.python = self.root / "venv/bin/python"
        required_directories = (
            self.carla / "PythonAPI/carla",
            self.carla / "Unreal/CarlaUnreal/Plugins/Carla/Resources",
            self.unreal / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS",
            self.build,
            self.tls,
            self.desktop,
        )
        for directory in required_directories:
            directory.mkdir(parents=True, exist_ok=True)
        required_files = (
            self.carla / "Unreal/CarlaUnreal/CarlaUnreal.uproject",
            self.tls / "server-cert.pem",
            self.tls / "server-key.pem",
        )
        for path in required_files:
            path.write_text("test fixture\n", encoding="utf-8")
        executables = (
            self.unreal
            / "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor",
            self.build / "carla-ego-runtime",
            self.build / "carla-viss-client",
        )
        for path in executables:
            path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            path.chmod(0o700)
        self.python.parent.mkdir(parents=True)
        self.python.symlink_to(sys.executable)
        self.icon = self.root / "Icon128.ppm"
        self.icon.write_bytes(
            b"P6\n128 128\n255\n" + bytes((24, 114, 184)) * 128 * 128
        )

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, *extra):
        return [
            sys.executable,
            str(INSTALLER),
            "--carla-root",
            str(self.carla),
            "--unreal-root",
            str(self.unreal),
            "--build-root",
            str(self.build),
            "--python",
            str(self.python),
            "--tls-root",
            str(self.tls),
            "--icon-source",
            str(self.icon),
            "--install-directory",
            str(self.desktop),
            "--state-directory",
            str(self.state),
            *extra,
        ]

    def test_installs_signed_icon_bundles_and_m62_launcher(self):
        subprocess.run(self.command(), check=True, capture_output=True, text=True)
        for app_name, expected_identifier in (
            (
                "CARLA Simulator.app",
                "io.github.alexmaninblack.carla-ego-runtime.route",
            ),
            (
                "CARLA Manual Drive.app",
                "io.github.alexmaninblack.carla-ego-runtime.manual-drive",
            ),
        ):
            app = self.desktop / app_name
            info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
            self.assertEqual(info["CFBundleIdentifier"], expected_identifier)
            self.assertTrue((app / "Contents/Resources/CARLA.icns").is_file())
            executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
            self.assertTrue(executable.stat().st_mode & stat.S_IXUSR)
            subprocess.run(
                ["codesign", "--verify", "--deep", "--strict", str(app)],
                check=True,
                capture_output=True,
                text=True,
            )

        manual = (self.state / "launchers/manual-drive.command").read_text()
        route = (self.state / "launchers/route.command").read_text()
        self.assertIn("config/m6_2_town10hd_handover.json", manual)
        self.assertIn("tools/launch_m6_1.py", manual)
        self.assertIn("config/m5_town10hd_route.json", route)
        self.assertIn(str(self.python), manual + route)
        self.assertNotIn("alexagizim", INSTALLER.read_text(encoding="utf-8"))
        self.assertEqual(
            stat.S_IMODE((self.state / "launchers/manual-drive.command").stat().st_mode),
            0o700,
        )

    def test_replace_is_explicit_and_preserves_previous_apps(self):
        subprocess.run(self.command(), check=True, capture_output=True, text=True)
        rejected = subprocess.run(self.command(), capture_output=True, text=True)
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("use --replace", rejected.stderr)
        subprocess.run(
            self.command("--replace"), check=True, capture_output=True, text=True
        )
        backups = list((self.state / "backups").glob("*/CARLA Simulator.app"))
        self.assertEqual(len(backups), 1)


if __name__ == "__main__":
    unittest.main()
