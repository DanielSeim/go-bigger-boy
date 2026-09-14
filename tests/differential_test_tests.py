#!/usr/bin/env python3
"""Unit tests for the runner differential comparator."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "scripts" / "differential_test.py"
SPEC = importlib.util.spec_from_file_location("differential_test", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class DifferentialTestTests(unittest.TestCase):
    def test_result_marker_ignores_serial_prefix(self) -> None:
        self.assertEqual(
            MODULE.result_marker("BBBB\x03PASS (Mooneye)\n"),
            "PASS (Mooneye)",
        )

    def test_identical_frames_compare_without_diff(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            pixels = bytes((0, 16, 32, 255, 240, 224))
            MODULE.write_ppm(root / "candidate.ppm", 2, 1, pixels)
            MODULE.write_ppm(root / "reference.ppm", 2, 1, pixels)
            self.assertIsNone(MODULE.compare_frames(
                root / "candidate.ppm", root / "reference.ppm",
                root / "diff.ppm"))
            self.assertFalse((root / "diff.ppm").exists())

    def test_different_frames_write_diff(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            MODULE.write_ppm(root / "candidate.ppm", 1, 1, bytes((0, 0, 0)))
            MODULE.write_ppm(root / "reference.ppm", 1, 1, bytes((255, 255, 255)))
            mismatch = MODULE.compare_frames(
                root / "candidate.ppm", root / "reference.ppm",
                root / "diff.ppm")
            self.assertIn("1 pixels differ", mismatch)
            self.assertTrue((root / "diff.ppm").exists())


if __name__ == "__main__":
    unittest.main()
