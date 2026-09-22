#!/usr/bin/env python3
"""Contract tests for model-matrix classification and diagnostics naming."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_matrix


class ModelMatrixContractTests(unittest.TestCase):
    def test_gbmicrotest_scope_is_limited_to_dmg(self) -> None:
        self.assertEqual(
            model_matrix.expected_models(
                "gbmicrotest", "gbmicrotest/halt_op_dupe_delay.gb"),
            {"dmg"},
        )

    def test_diagnostic_names_are_stable_and_safe(self) -> None:
        self.assertEqual(
            model_matrix.diagnostic_name(
                "mooneye-wilbertpol",
                "acceptance/gpu/intr_2_mode0_scx1_timing_nops.gb",
                "cgb-c",
            ),
            "mooneye-wilbertpol__acceptance_gpu_intr_2_mode0_scx1_timing_nops.gb__cgb-c",
        )

    def test_failure_summary_keeps_failure_and_cpu_state(self) -> None:
        output = (
            "FAIL (Mooneye result registers)\n"
            "PC=0040 SP=fffe AF=0000 cycles=123\n"
            "Video registers: LCDC=91\n"
        )
        self.assertEqual(
            model_matrix.summarize_runner_output(output),
            "FAIL (Mooneye result registers); PC=0040 SP=fffe AF=0000 cycles=123",
        )


if __name__ == "__main__":
    unittest.main()
