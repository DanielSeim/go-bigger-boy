#!/usr/bin/env python3
"""Execute a synthetic logo-free homebrew cartridge through DMG startup."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = str(Path(sys.argv.pop(1)).resolve())


def cartridge():
    rom = bytearray(0x8000)
    rom[0x100:0x103] = bytes([0xC3, 0x50, 0x01])  # JP 0150
    program = bytearray()
    for byte in b"Passed\n":
        program.extend([0x3E, byte, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
                        0xF0, 0x02, 0xCB, 0x7F, 0x20, 0xFA])
    program.extend([0x18, 0xFE])
    rom[0x150:0x150 + len(program)] = program
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 0xFF
    return rom


class DmgBootCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gbb-dmg-boot-cli-")
        self.addCleanup(self.directory.cleanup)
        self.rom = Path(self.directory.name) / "original-homebrew.gb"
        self.rom.write_bytes(cartridge())

    def run_rom(self, *args):
        return subprocess.run([RUNNER, str(self.rom), "--protocol", "serial",
                               "--max-cycles", "6000000", *args],
                              capture_output=True, text=True, timeout=30)

    def test_cartridge_runs_from_cold_boot(self):
        result = self.run_rom("--model", "dmg", "--dmg-boot")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS (serial)", result.stdout)

    def test_post_boot_remains_default(self):
        result = self.run_rom("--model", "dmg")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_invalid_checksum_never_executes_cartridge(self):
        rom = cartridge()
        rom[0x14D] ^= 1
        self.rom.write_bytes(rom)
        result = self.run_rom("--model", "dmg", "--dmg-boot")
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("Passed", result.stdout)
        self.assertIn("TIMEOUT", result.stderr)

    def test_modes_are_mutually_exclusive(self):
        result = self.run_rom("--model", "dmg", "--dmg-boot", "--diagnostic-boot")
        self.assertEqual(result.returncode, 2)
        self.assertIn("mutually exclusive", result.stderr)

    def test_sgb_is_not_treated_as_dmg(self):
        result = self.run_rom("--model", "sgb", "--dmg-boot")
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires the DMG", result.stderr)


if __name__ == "__main__":
    unittest.main()
