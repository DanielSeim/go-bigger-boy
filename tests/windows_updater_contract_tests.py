"""Native Windows installation, helper replacement and relaunch contract."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import shutil
import tempfile
import time
import unittest
import zipfile


class WindowsUpdaterContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="GBB update ü ")
        self.addCleanup(self.cleanup_installation)
        self.base = Path(self.temporary.name)
        self.root = self.base / "installed application"
        self.root.mkdir()
        self.archive = self.base / "download cache" / "release.zip"
        self.archive.parent.mkdir()
        self.settings = b"[user]\r\nvolume=37\r\n"
        (self.root / "settings.ini").write_bytes(self.settings)
        self.old_application = LAUNCHER.read_bytes() + b"old application"
        (self.root / "gbb.exe").write_bytes(self.old_application)
        for dll in LAUNCHER.parent.glob("*.dll"):
            shutil.copyfile(dll, self.root / dll.name)
        # An invalid legacy helper must never be launched by the new frontend.
        (self.root / "gbb-updater.exe").write_bytes(b"old updater")
        (self.root / "game.sav").write_bytes(b"user save")

    def cleanup_installation(self):
        # The relaunched app writes its marker just before process shutdown;
        # Windows can briefly keep its executable/DLLs locked after that.
        deadline = time.monotonic() + 5
        while True:
            try:
                self.temporary.cleanup()
                return
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)

    def package(self, include_helper=True):
        with zipfile.ZipFile(self.archive, "w") as package:
            package.write(LAUNCHER, "gbb.exe")
            if include_helper:
                package.write(UPDATER, "gbb-updater.exe")
            for dll in LAUNCHER.parent.glob("*.dll"):
                package.write(dll, dll.name)
            package.writestr("settings.ini", b"package defaults")
            package.writestr("data/payload.txt", b"new payload")
        return hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def launch(self, digest):
        return subprocess.run(
            [str(self.root / "gbb.exe"), str(self.archive), str(self.root),
             str(self.root / "gbb.exe"), digest],
            capture_output=True, timeout=30,
        )

    def assert_unchanged(self):
        self.assertEqual((self.root / "gbb.exe").read_bytes(), self.old_application)
        self.assertEqual((self.root / "gbb-updater.exe").read_bytes(), b"old updater")
        self.assertEqual((self.root / "settings.ini").read_bytes(), self.settings)
        self.assertFalse((self.root / "restarted.txt").exists())
        self.assertTrue(self.archive.exists())

    def test_replaces_legacy_helper_and_relaunches_with_user_settings(self):
        self.install_and_check()

    def test_install_does_not_require_an_existing_helper(self):
        (self.root / "gbb-updater.exe").unlink()
        self.install_and_check()

    def install_and_check(self):
        result = self.launch(self.package())
        self.assertEqual(result.returncode, 0, result.stderr)
        marker = self.root / "restarted.txt"
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if marker.exists() and marker.read_bytes() == self.settings:
                break
            time.sleep(0.05)
        self.assertTrue(marker.exists(), "updated app did not relaunch")
        self.assertEqual(marker.read_bytes(), self.settings)
        self.assertEqual((self.root / "gbb.exe").read_bytes(), LAUNCHER.read_bytes())
        self.assertEqual((self.root / "gbb-updater.exe").read_bytes(), UPDATER.read_bytes())
        self.assertEqual((self.root / "settings.ini").read_bytes(), self.settings)
        self.assertEqual((self.root / "game.sav").read_bytes(), b"user save")
        self.assertEqual((self.root / "data/payload.txt").read_bytes(), b"new payload")
        self.assertFalse(self.archive.exists())
        # The running helper and its DLLs stay outside the install directory.
        self.assertEqual(len(list(self.archive.parent.glob("helper-*/gbb-updater.exe"))), 1)

    def test_rejects_changed_archive_before_starting_helper(self):
        digest = self.package()
        with self.archive.open("ab") as archive:
            archive.write(b"changed after download")
        result = self.launch(digest)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"SHA-256", result.stderr)
        self.assert_unchanged()
        self.assertFalse(list(self.archive.parent.glob("helper-*")))

    def test_rejects_package_without_helper_before_installation(self):
        result = self.launch(self.package(include_helper=False))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"native helper", result.stderr)
        self.assert_unchanged()
        self.assertFalse(list(self.archive.parent.glob("helper-*")))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", type=Path, required=True)
    parser.add_argument("--updater", type=Path, required=True)
    arguments = parser.parse_args()
    LAUNCHER = arguments.launcher.resolve()
    UPDATER = arguments.updater.resolve()
    unittest.main(argv=[__file__])
