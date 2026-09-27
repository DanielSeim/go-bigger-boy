#!/usr/bin/env python3
"""Offline contract checks for the development-only DSP fixture protocol."""

from __future__ import annotations

import importlib.util
import hashlib
from pathlib import Path
import subprocess
import struct
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

    def test_reference_pinned_dynamic_pcm(self) -> None:
        # SHA-256 covers every native-rate stereo frame, not just startup.
        # These synthetic traces matched the independent S-DSP byte-for-byte.
        fixtures = {
            "register_writes.txt": (96, "30dbe2bb58107dd60037bf6b48d8237b321a74d26b68c012d3bf435efe5dc964"),
            "adsr_transition.txt": (96, "73e65a02b8a5a2e68c4a5df3d56c343537739930329a8381d9dad7f67ccb0e06"),
            "brr_loop.txt": (96, "fa80b891cb9b312c12bd0bb21449049824aef7e3f9422d9ea5b69738a1ba66f3"),
            "noise.txt": (88, "2ee776e8ad72d6f48c98cb9f43b15ea821909b70a7eb42565dff62f5317dbb8c"),
            "noise_voice1.txt": (80, "076599413df1f83514e6c0b35d7ac76d13306737159ba692979e7f13f46f50e0"),
            "noise_modulation.txt": (128, "fb62b3e6da91978b31354553318a18d7f7e2dbfc79bc4d35e1d1d82b45f515bf"),
            "varying_brr.txt": (128, "adb409f348dc28fb1871316dd6a0a483331557f50392ba569fb89c5546cd6de4"),
            "varying_two_voices.txt": (128, "165ce0395a0ee1c949a0b6cef6f91f2005d0d60dd18c472ef77dd97de45dfff0"),
            "pitch_modulation.txt": (128, "a6359dea807db086f1a5197e3122d5ec938e8102dc50e195bd04f8f50fbc61ac"),
            "echo_read_only.txt": (24, "1ba774eab1a1b9ffafe20cbad859eb724d09458b52408d8713d39a16b64efa4c"),
            "echo_fir_taps.txt": (32, "31c095adb94a168beca128f9eadeeea0de6008606b12169fb192923da27fd39b"),
            "echo_feedback.txt": (84, "27c79d3d7fcf27a098294bdd0425f46ef20616eac4833cf6a2a411c63f6f2933"),
            "echo_delay_wrap.txt": (570, "94d584ac22eba9aa0ce7dbcef9c59db1c4e3efb55f19583e7756a63066186f35"),
            "echo_reconfigure.txt": (20, "8f91a403a648510e98413607dc32c9679f5689ac2490189426cb99d8fd9c2a4a"),
            "echo_address_wrap.txt": (68, "a262d21aa092e4cbeaf9afdcbbaa4439270b4c7ad01600d9c7b9d97939dcfd80"),
        }
        for name, (count, expected) in fixtures.items():
            with self.subTest(fixture=name):
                stimulus = (self.fixture.parent / name).read_bytes()
                samples = MODULE.run_fixture(self.runner, stimulus)
                self.assertEqual(len(samples), count)
                pcm = b"".join(struct.pack("<hh", *sample) for sample in samples)
                self.assertEqual(hashlib.sha256(pcm).hexdigest(), expected)

    def test_rejects_malformed_stimuli(self) -> None:
        malformed = subprocess.run(
            [str(self.runner)], input=b"ram 0x10000 1\nstep 1\n",
            capture_output=True, check=False,
        )
        self.assertEqual(malformed.returncode, 2)
        self.assertIn(b"fixture line 1", malformed.stderr)
        invalid_register = subprocess.run(
            [str(self.runner)], input=b"reg 0x80 1\nstep 1\n",
            capture_output=True, check=False,
        )
        self.assertEqual(invalid_register.returncode, 2)
        self.assertIn(b"fixture line 1", invalid_register.stderr)

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
