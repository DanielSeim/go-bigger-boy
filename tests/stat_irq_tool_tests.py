#!/usr/bin/env python3
"""Offline checks using independently authored, logo-free result stubs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROBE = Path(sys.argv.pop(1)).resolve()
TOOL = Path(__file__).resolve().parents[1] / "scripts/validate_stat_irq.py"


class StatIrqToolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gbb-stat-tool-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "report.json"

    def fixture(self, result=0xE0, expected="E0"):
        # A result stub only, not a simulated hardware STAT timing fixture.
        rom = bytearray(0x8000)
        rom[0x100:0x105] = bytes([0x3E, result, 0xC3, 0x00, 0x70])
        path = self.root / f"lycint152_stub_dmg08_cgb04c_out{expected}.gbc"
        path.write_bytes(rom)
        return path

    def run_tool(self, *arguments):
        return subprocess.run([sys.executable, str(TOOL), "--probe", str(PROBE),
                               "--rom-directory", str(self.root), "--output", str(self.output),
                               *arguments], capture_output=True, text=True, timeout=30)

    def test_pass_and_no_input_mutation(self):
        fixture = self.fixture()
        original = fixture.read_bytes()
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(self.output.read_text())
        self.assertEqual(report["passed"], 1)
        self.assertEqual(report["cases"][0]["actual"], 0xE0)
        self.assertEqual(len(report["cases"][0]["rom"]["sha256"]), 64)
        self.assertEqual(fixture.read_bytes(), original)
        self.assertFalse(fixture.with_suffix(".sav").exists())
        self.assertNotIn(str(self.root), self.output.read_text())

    def test_mismatch_is_failure(self):
        self.fixture(result=0xE2)
        self.assertEqual(self.run_tool().returncode, 1)
        self.assertEqual(json.loads(self.output.read_text())["cases"][0]["status"], "fail")

    def test_missing_inputs_are_errors(self):
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertFalse(self.output.exists())

    def test_refuses_overwrite(self):
        self.fixture()
        self.output.write_text("keep me")
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertEqual(self.output.read_text(), "keep me")

    def test_rejects_wrong_boot_model(self):
        self.fixture()
        self.assertEqual(self.run_tool("--model", "cgb-c", "--dmg-boot").returncode, 2)

    def test_cgb_filename_selection(self):
        self.fixture()
        result = self.run_tool("--model", "cgb-c")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.output.read_text())["model"], "cgb-c")

    def test_probe_failure_is_not_a_pass(self):
        path = self.fixture()
        path.write_bytes(b"bad")
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertFalse(self.output.exists())

    def test_invalid_result_name_is_error(self):
        self.fixture(expected="ZZ")
        self.assertEqual(self.run_tool().returncode, 2)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
