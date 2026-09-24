import importlib.util
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "scripts" / "check_android_sgb_performance.py"
SPEC = importlib.util.spec_from_file_location("sgb_performance_gate", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def log(*values: float, mode: str = "nearest") -> str:
    return "\n".join(
        f"09-24 I/SDL: GBB frame_timing fps={value} sgb_frames=60 "
        "core_step_avg_us=4000 sgb_compose_avg_us=40 "
        "sgb_transform_avg_us=100 sgb_upload_avg_us=200 "
        f"sgb_present_avg_us=1800 mode={mode}"
        for value in values)


class AndroidSgbPerformanceGateTests(unittest.TestCase):
    def test_accepts_steady_state_after_warmup(self):
        passed, report = MODULE.evaluate(log(35, 59.5, 60, 59.1), 59, 1, 3)
        self.assertTrue(passed, report)
        self.assertIn("sgb_compose_avg_us=40", report)

    def test_rejects_sustained_slow_windows(self):
        passed, report = MODULE.evaluate(log(60, 50, 51, 52), 59, 1, 3)
        self.assertFalse(passed)
        self.assertIn("worst_fps=50.00", report)

    def test_tolerates_one_jitter_window_but_reports_it(self):
        passed, report = MODULE.evaluate(log(60, 59.5, 57, 61), 59, 1, 3)
        self.assertTrue(passed, report)
        self.assertIn("worst_fps=57.00", report)

    def test_filters_mixed_modes(self):
        contents = log(40, 40, 40, mode="voxel") + "\n" + log(
            60, 60, 60, mode="nearest")
        passed, report = MODULE.evaluate(contents, 59, 0, 3, "nearest")
        self.assertTrue(passed, report)
        self.assertIn("mode=nearest", report)

    def test_rejects_missing_sgb_windows(self):
        passed, report = MODULE.evaluate(
            log(60, 60).replace("sgb_frames=60", "sgb_frames=0"), 59, 0, 1)
        self.assertFalse(passed)
        self.assertIn("only 0", report)


if __name__ == "__main__":
    unittest.main()
