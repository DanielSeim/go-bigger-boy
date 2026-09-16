#!/usr/bin/env python3
"""Tests for the dependency-free voxel screenshot comparison command."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "scripts" / "compare_voxel_screenshots.py"
SPEC = importlib.util.spec_from_file_location("compare_voxel_screenshots", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_ppm(path: Path, pixels: bytes) -> None:
    path.write_bytes(b"P6\n2 1\n255\n" + pixels)


class VoxelVisualRegressionTests(unittest.TestCase):
    def test_identical_representative_screenshot_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference.ppm"
            actual = root / "actual.ppm"
            pixels = bytes((0, 0, 0, 0, 194, 255))
            write_ppm(reference, pixels)
            write_ppm(actual, pixels)
            report = MODULE.compare_images(reference, actual)
            self.assertTrue(report["passed"])
            self.assertEqual(report["mismatched_pixels"], 0)

    def test_report_catches_geometry_or_viewport_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference.ppm"
            actual = root / "actual.ppm"
            write_ppm(reference, bytes((0, 0, 0, 0, 194, 255)))
            write_ppm(actual, bytes((0, 0, 0, 1, 194, 255)))
            report = MODULE.compare_images(reference, actual)
            self.assertFalse(report["passed"])
            self.assertEqual(report["first_mismatch"], [1, 0])

    def test_small_capture_noise_can_be_explicitly_tolerated(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference.ppm"
            actual = root / "actual.ppm"
            write_ppm(reference, bytes((0, 0, 0, 10, 20, 30)))
            write_ppm(actual, bytes((0, 0, 0, 11, 19, 30)))
            report = MODULE.compare_images(reference, actual, 1)
            self.assertTrue(report["passed"])

    def test_dimension_drift_is_reported_before_pixel_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reference = root / "reference.ppm"
            actual = root / "actual.ppm"
            reference.write_bytes(b"P6\n2 1\n255\n" + bytes((0, 0, 0, 0, 0, 0)))
            actual.write_bytes(b"P6\n1 1\n255\n" + bytes((0, 0, 0)))
            report = MODULE.compare_images(reference, actual)
            self.assertFalse(report["passed"])
            self.assertEqual(report["reason"], "dimensions")


if __name__ == "__main__":
    unittest.main()
