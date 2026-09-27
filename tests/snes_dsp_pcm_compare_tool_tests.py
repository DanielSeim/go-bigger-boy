#!/usr/bin/env python3
"""Offline contract checks for the development-only DSP fixture protocol."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "compare_snes_dsp_pcm.py"
RUNNER_PATH = Path(sys.argv[1]).resolve()
SPEC = importlib.util.spec_from_file_location("compare_snes_dsp_pcm", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PcmCompareToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner = RUNNER_PATH
        cls.fixture = ROOT / "tests" / "fixtures" / "sgb" / "dsp" / "single_voice.txt"

    def test_active_stimulus_is_repeatable(self) -> None:
        stimulus = self.fixture.read_bytes()
        first = MODULE.run_fixture(self.runner, stimulus)
        second = MODULE.run_fixture(self.runner, stimulus)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 64)
        self.assertTrue(any(left or right for left, right in first))

    def test_reference_pinned_startup_samples(self) -> None:
        fixture_dir = self.fixture.parent
        for name, count, first_audible in (
            ("silence.txt", 64, None),
            ("single_voice.txt", 64, (8, (178, 90))),
            ("key_off.txt", 80, (8, (1000, 1000))),
            ("two_voices.txt", 64, (8, (357, 90))),
        ):
            with self.subTest(fixture=name):
                samples = MODULE.run_fixture(self.runner, (fixture_dir / name).read_bytes())
                self.assertEqual(len(samples), count)
                audible = next(
                    ((index, sample) for index, sample in enumerate(samples)
                     if sample != (0, 0)), None)
                self.assertEqual(audible, first_audible)

    def test_rejects_malformed_and_unsupported_stimuli(self) -> None:
        malformed = subprocess.run(
            [str(self.runner)], input=b"ram 0x10000 1\nstep 1\n",
            capture_output=True, check=False,
        )
        self.assertEqual(malformed.returncode, 2)
        self.assertIn(b"fixture line 1", malformed.stderr)
        unsupported = subprocess.run(
            [str(self.runner)], input=b"reg 0x3d 1\nstep 1\n",
            capture_output=True, check=False,
        )
        self.assertEqual(unsupported.returncode, 3)
        self.assertIn(b"unsupported DSP mode", unsupported.stderr)

    def test_comparison_detects_first_changed_stereo_frame(self) -> None:
        left = [(0, 0), (12, -4), (20, 6)]
        right = [(0, 0), (12, -4), (21, 6)]
        self.assertEqual(MODULE.first_mismatch(left, right), (2, left[2], right[2]))
        self.assertIsNone(MODULE.first_mismatch(left, left))

    def test_exact_comparison_passes_for_identical_runners(self) -> None:
        self.assertTrue(MODULE.compare(self.fixture, self.runner, self.runner, 1))


if __name__ == "__main__":
    sys.argv = [sys.argv[0]]
    unittest.main()
