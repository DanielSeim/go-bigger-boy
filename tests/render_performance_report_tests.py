#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("render_performance_report.py")
SPEC = importlib.util.spec_from_file_location("render_performance_report", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class RenderPerformanceReportTests(unittest.TestCase):
    def setUp(self):
        self.baseline = {
            "minimum_ratio": 0.65,
            "platforms": {
                "linux": {"nearest": 100, "voxel": 40, "voxel_shape": 40, "voxel_popup": 20}
            },
        }

    def test_passes_within_variance_margin(self):
        report = {
            "modes": [
                {"mode": "nearest", "fps": 70},
                {"mode": "voxel", "fps": 30},
                {"mode": "voxel_shape", "fps": 30},
                {"mode": "voxel_popup", "fps": 15},
            ]
        }
        result = MODULE.evaluate_report(report, self.baseline, "linux")
        self.assertEqual(result["status"], "pass")

    def test_fails_a_mode_below_the_baseline_margin(self):
        report = {
            "modes": [
                {"mode": "nearest", "fps": 70},
                {"mode": "voxel", "fps": 20},
                {"mode": "voxel_shape", "fps": 30},
                {"mode": "voxel_popup", "fps": 15},
            ]
        }
        result = MODULE.evaluate_report(report, self.baseline, "linux")
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["results"][1]["status"], "fail")

    def test_rejects_unknown_platform(self):
        with self.assertRaises(ValueError):
            MODULE.evaluate_report({"modes": []}, self.baseline, "windows")


if __name__ == "__main__":
    unittest.main()
