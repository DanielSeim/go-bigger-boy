#!/usr/bin/env python3
"""Probe/comparison regression tests using only original logo-free fixtures."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROBE = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("compare_dmg_boot", ROOT / "scripts/compare_dmg_boot.py")
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gbb-dmg-probe-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.rom = self.root / "homebrew.gb"
        image = bytearray(32768)
        image[0x100:0x105] = bytes([0x3E, 0x42, 0xEA, 0x00, 0xC0])
        image[0x105:0x107] = bytes([0x18, 0xFE])
        image[0x14D] = (-sum(image[0x134:0x14D]) - 25) & 255
        self.rom.write_bytes(image)

    def probe(self, *options):
        return subprocess.run([PROBE, str(self.rom), *options], capture_output=True,
                              text=True, timeout=30)

    def snapshot(self):
        result = self.probe()
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_exact_boundary_and_followup(self):
        result = self.probe("--run-cycles", "80")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["handoff"]["cpu"]["pc"], 256)
        self.assertEqual(data["handoff"]["wram"][0], 0)
        self.assertEqual(data["followup"]["wram"][0], 0x42)
        self.assertEqual(data["handoff"]["io"][0x41], 0x85)
        self.assertEqual(len(data["handoff"]["apu_clocks"]), 18)
        self.assertEqual(data["handoff"]["divider_counter"], 0xABC8)
        self.assertEqual(data["handoff"]["ppu_dot"], 396)
        self.assertEqual(data["handoff"]["serial_phase"], 452)
        clocks = data["handoff"]["apu_clocks"]
        self.assertEqual((clocks[0], clocks[3], clocks[4], clocks[6]), (1, 30, 2, 0))
        self.assertGreater(data["audio"]["boot"]["samples"], 0)
        self.assertGreater(data["audio"]["followup"]["samples"], 0)
        self.assertLessEqual(data["audio"]["followup"]["peak"], 8)

    def test_opaque_reference_execution(self):
        # Original three-instruction fixture, not any downloaded firmware.
        boot = bytearray(256)
        boot[:5] = bytes([0x3E, 0x01, 0xC3, 0xFE, 0x00])
        boot[0xFE:] = bytes([0xE0, 0x50])
        path = self.root / "test-boot.bin"
        path.write_bytes(boot)
        result = self.probe("--boot-rom", str(path))
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)["handoff"]
        self.assertEqual(data["cycles"], 36)
        self.assertEqual(data["cpu"]["pc"], 256)
        self.assertEqual(data["io"][0], 0xCF)

        offset = self.probe("--boot-rom", str(path), "--cold-clock-cycles", "4")
        self.assertEqual(offset.returncode, 0, offset.stderr)
        experiment = json.loads(offset.stdout)
        self.assertEqual(experiment["cold_clock_cycles"], 4)
        self.assertEqual(experiment["handoff"]["cycles"], 36)
        self.assertEqual(experiment["handoff"]["divider_counter"], data["divider_counter"] + 4)
        self.assertEqual(experiment["handoff"]["cpu"], data["cpu"])

    def test_completed_frame_alignment_leaves_handoff_untouched(self):
        unaligned = self.snapshot()
        result = self.probe("--align-frame")
        self.assertEqual(result.returncode, 0, result.stderr)
        aligned = json.loads(result.stdout)
        self.assertEqual(aligned["handoff"], unaligned["handoff"])
        self.assertEqual(aligned["followup"]["ppu_mode"], 1)
        self.assertEqual(aligned["followup"]["io"][0x44], 144)
        self.assertGreater(aligned["followup"]["cycles"], aligned["handoff"]["cycles"])

    def test_disabled_lcd_alignment_fails_with_a_bound(self):
        image = bytearray(self.rom.read_bytes())
        image[0x100:0x105] = bytes([0xAF, 0xE0, 0x40, 0x18, 0xFE])
        self.rom.write_bytes(image)
        result = self.probe("--run-cycles", "80", "--align-frame")
        self.assertEqual(result.returncode, 2)
        self.assertIn("frame alignment timed out", result.stderr)

    def test_failures_are_not_partial_success(self):
        for arguments in (("--max-cycles", "1"), ("--max-cycles", "-1"),
                          ("--run-cycles", "12x"), ("--cold-clock-cycles", "17"),
                          ("--unknown", "0")):
            result = self.probe(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")
        path = self.root / "bad-boot.bin"
        path.write_bytes(bytes(255))
        self.assertEqual(self.probe("--boot-rom", str(path)).returncode, 2)

    def test_comparison_does_not_hide_contract_failures(self):
        original = self.snapshot()
        for field in ("cpu", "io", "wram", "oam", "hram"):
            changed = deepcopy(original)
            if field == "cpu": changed["handoff"][field]["a"] ^= 1
            else: changed["handoff"][field][0] ^= 1
            self.assertEqual(comparison.compare(changed, original)["stable_contract"], "fail", field)
        self.assertEqual(comparison.compare(original, original)["stable_contract"], "pass")

    def test_phase_and_graphics_differences_stay_visible(self):
        original = self.snapshot()
        changed = deepcopy(original)
        changed["handoff"]["divider_counter"] ^= 1
        changed["handoff"]["vram"][0] = 0x5A
        changed["handoff"]["hram"][0x7A] = 0x5A
        report = comparison.compare(changed, original)
        self.assertEqual(report["stable_contract"], "pass")
        self.assertFalse(report["exact_snapshot_equivalence"])
        self.assertEqual(report["ram"]["vram"]["different_bytes"], 1)
        self.assertEqual(report["ram"]["vram"]["ranges"][0]["first"], "8000")
        self.assertNotIn("vram", report["followup"]["reference"])

    def test_truncated_snapshots_are_rejected(self):
        original = self.snapshot()
        changed = deepcopy(original)
        changed["handoff"]["io"].pop()
        with self.assertRaises(ValueError):
            comparison.compare(changed, original)

    def test_cli_records_failure_and_refuses_overwrite(self):
        boot = bytearray(256)
        boot[:5] = bytes([0x3E, 0x01, 0xC3, 0xFE, 0x00])
        boot[0xFE:] = bytes([0xE0, 0x50])
        path = self.root / "fixture.bin"
        path.write_bytes(boot)
        output = self.root / "report.json"
        command = [sys.executable, str(ROOT / "scripts/compare_dmg_boot.py"),
                   "--probe", PROBE, "--reference-boot", str(path),
                   "--rom", str(self.rom), "--run-cycles", "0", "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["reference_boot"]["size"], 256)
        before = output.read_bytes()
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(output.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
