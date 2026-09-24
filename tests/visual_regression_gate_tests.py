#!/usr/bin/env python3
"""Tests for the per-backend visual regression gate."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "visual_regression_gate.py"
SPEC = importlib.util.spec_from_file_location("visual_regression_gate", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_ppm(path: Path, pixels: bytes) -> None:
    path.write_bytes(b"P6\n2 1\n255\n" + pixels)


class VisualRegressionGateTests(unittest.TestCase):
    def test_backend_baseline_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference"
            actual = root / "actual"
            reference.mkdir()
            actual.mkdir()
            pixels = bytes((0, 0, 0, 194, 194, 194))
            write_ppm(reference / "voxel.ppm", pixels)
            write_ppm(actual / "voxel.ppm", pixels)
            report = MODULE.compare_set(reference, actual, "sdl-software",
                                        ["voxel"], 0, 0.0)
            self.assertEqual(report["status"], "pass")

    def test_gate_rejects_missing_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference"
            actual = root / "actual"
            reference.mkdir()
            actual.mkdir()
            report = MODULE.compare_set(reference, actual, "webgl",
                                        ["voxel_popup"], 0, 0.0)
            self.assertEqual(report["status"], "fail")
            self.assertEqual(report["modes"][0]["reason"], "missing_capture")

    def test_gate_enforces_mismatch_budget_after_channel_tolerance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference"
            actual = root / "actual"
            reference.mkdir()
            actual.mkdir()
            write_ppm(reference / "voxel.ppm", bytes((0, 0, 0, 0, 0, 0)))
            write_ppm(actual / "voxel.ppm", bytes((1, 0, 0, 1, 0, 0)))
            report = MODULE.compare_set(reference, actual, "webgl", ["voxel"],
                                        1, 0.25)
            self.assertEqual(report["status"], "pass")
            report = MODULE.compare_set(reference, actual, "webgl", ["voxel"],
                                        0, 0.25)
            self.assertEqual(report["status"], "fail")


if __name__ == "__main__":
    unittest.main()
