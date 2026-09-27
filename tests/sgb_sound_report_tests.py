"""Opt-in SGB sound reports contain timing and hashes, never sample bytes."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sgb_trace_fixture_test import load_fixture  # noqa: E402


RUNNER = Path(sys.argv.pop()) if len(sys.argv) > 1 else None


class SgbSoundReportTests(unittest.TestCase):
    def test_synthetic_sound_command_is_reported(self) -> None:
        if RUNNER is None:
            self.skipTest("runner path not supplied")
        fixture = Path(__file__).parent / "fixtures/sgb/trace_fixture.hex"
        rom_bytes = bytearray(load_fixture(fixture))
        # The clean-room MLT_REQ sender reads its packet from 0x0150. Replace
        # only that synthetic packet with SOUND; no commercial ROM is needed.
        rom_bytes[0x150:0x160] = bytes([0x41, 3, 4, 0x0C, 2] + [0] * 11)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rom = root / "sound.gb"
            rom.write_bytes(rom_bytes)
            report = root / "sound.txt"
            result = subprocess.run(
                [str(RUNNER), str(rom), "--model", "sgb2", "--frames", "2",
                 "--frame-output", str(root / "frame.ppm"),
                 "--sgb-sound-report", str(report), "--max-cycles", "500000"],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = report.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "GBB SGB sound report v1")
            self.assertEqual(len(lines), 2)
            self.assertIn("revision=1 effect_a=3 effect_b=4 attributes=12 score=2",
                          lines[1])
            self.assertNotIn("packet=", lines[1])

            rejected = subprocess.run(
                [str(RUNNER), str(rom), "--model", "dmg", "--frames", "1",
                 "--frame-output", str(root / "dmg.ppm"),
                 "--sgb-sound-report", str(root / "invalid.txt")],
                capture_output=True, text=True, timeout=10, check=False)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("requires an SGB or SGB2", rejected.stderr)


if __name__ == "__main__":
    unittest.main()
