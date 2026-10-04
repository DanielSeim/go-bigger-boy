#!/usr/bin/env python3
"""Logo-free serial diagnostics/tool regressions; no external boot dependency."""
from copy import deepcopy
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import compare_dmg_boot_serial as tool


class SerialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="gbb-dmg-serial-")
        cls.root = Path(cls.directory.name)
        cls.rom = cls.root / "homebrew.gb"
        image = bytearray(32768)
        image[0x100] = 0x76
        image[0x14D] = (-sum(image[0x134:0x14D]) - 25) & 255
        cls.rom.write_bytes(image)
        cls.boot = cls.root / "gbb-boot.bin"
        # GBB's generated original firmware, not a Nintendo fixture.
        cls.boot.write_bytes(bytes(int(b, 16) for b in re.findall(
            r"0x([0-9A-F]{2})", (ROOT / "firmware/gameboy/dmg_boot_image.hpp").read_text())))
        cls.snapshot = cls.probe("--serial-check")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    @classmethod
    def probe(cls, *args):
        result = subprocess.run([PROBE, str(cls.rom), *args], capture_output=True,
                                text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stderr)
        return json.loads(result.stdout)

    def test_bit_edges_interrupts_and_restoration(self):
        self.assertEqual(self.snapshot["handoff"]["serial_phase"], 452)
        self.assertEqual(tool.validate(self.snapshot), [])
        self.assertEqual(self.snapshot["serial_check"]["internal"][0]["edges"][0][0], 60)
        self.assertEqual(self.snapshot["serial_check"]["internal"][0]["edges"][-1][0], 3644)

    def test_probe_checks_do_not_change_execution_or_audio(self):
        plain = self.probe("--run-cycles", "1000")
        traced = self.probe("--run-cycles", "1000", "--serial-check")
        del traced["serial_check"]
        self.assertEqual(plain, traced)

    def test_both_checksum_paths_have_same_serial_boundary(self):
        image = bytearray(self.rom.read_bytes())
        original = bytes(image)
        image[0x134] = image[0x14D]
        image[0x14D] = 0
        try:
            self.rom.write_bytes(image)
            zero = self.probe("--serial-check")
        finally:
            self.rom.write_bytes(original)
        self.assertEqual(zero["handoff"]["cpu"]["f"], 0x80)
        self.assertEqual(zero["serial_check"], self.snapshot["serial_check"])

    def test_protocol_mutations_cannot_pass_even_when_both_runs_match(self):
        changed = deepcopy(self.snapshot)
        changed["handoff"]["serial_bits"] = 1
        self.assertEqual(tool.comparison(changed, changed)["status"], "fail")
        for stage, key in (("internal", "restore_exact"), ("external", "waits"),
                           ("linked", "completion_once")):
            changed = deepcopy(self.snapshot)
            row = changed["serial_check"][stage]
            if stage != "external":
                row = row[0]
            row[key] = 0
            self.assertEqual(tool.comparison(changed, changed)["status"], "fail")
        for index in range(4):
            changed = deepcopy(self.snapshot)
            changed["serial_check"]["internal"][0]["edges"][0][index] ^= 1
            self.assertEqual(tool.comparison(changed, changed)["status"], "fail")
        changed = deepcopy(self.snapshot)
        changed["serial_check"]["external"]["edges"][2][0] ^= 1
        self.assertEqual(tool.comparison(changed, changed)["status"], "fail")
        changed = deepcopy(self.snapshot)
        changed["serial_check"]["linked"][1]["edges"][1][0] = 2
        self.assertEqual(tool.comparison(changed, changed)["status"], "fail")

    def test_phase_mismatch_is_not_hidden_by_protocol_correctness(self):
        changed = deepcopy(self.snapshot)
        changed["handoff"]["serial_phase"] = 392
        for row in changed["serial_check"]["internal"]:
            row["phase"] = (392 + row["idle"]) % 512
            for bit, edge in enumerate(row["edges"]):
                edge[0] = 512 - row["phase"] + bit * 512
        self.assertEqual(tool.validate(changed), [])
        self.assertEqual(tool.comparison(changed, self.snapshot)["status"], "fail")

    def test_missing_cases_and_nonzero_offsets_rejected(self):
        for stage in ("internal", "linked"):
            changed = deepcopy(self.snapshot)
            changed["serial_check"][stage].pop()
            with self.assertRaises(ValueError):
                tool.validate(changed)
        changed = deepcopy(self.snapshot)
        changed["cold_clock_cycles"] = 4
        with self.assertRaises(ValueError):
            tool.validate(changed)
        changed["cold_clock_cycles"] = 0
        changed["handoff"]["serial_phase"] = 512
        with self.assertRaises(ValueError):
            tool.validate(changed)

    def test_cli_provenance_and_safe_report(self):
        output = self.root / "report.json"
        command = [sys.executable, str(ROOT / "scripts/compare_dmg_boot_serial.py"),
                   "--probe", PROBE, "--reference-boot", str(self.boot),
                   "--rom", str(self.rom), "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["cold_clock_cycles"], 0)
        self.assertEqual(report["titles"][0]["handoff_phase"], {"replacement": 452, "reference": 452})
        self.assertEqual(report["analysis_tool"]["sha256"], tool.fingerprint(
            ROOT / "scripts/compare_dmg_boot_serial.py")["sha256"])
        for private in ('"vram"', '"wram"', '"hram"', '"apu_writes"'):
            self.assertNotIn(private, output.read_text())
        before = output.read_bytes()
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(output.read_bytes(), before)

    def test_cli_invalid_inputs_do_not_create_reports(self):
        output = self.root / "invalid-report.json"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/compare_dmg_boot_serial.py"),
                                 "--probe", PROBE, "--reference-boot", str(self.boot),
                                 "--rom", str(self.rom), "--max-cycles", "1", "--output", str(output)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
