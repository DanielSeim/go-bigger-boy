#!/usr/bin/env python3
"""ROM-free provenance, ordinal, and timer-history contracts."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_sgb_phase_origin import compare, sequence, default_timer_interval, timer_model


def capture(source, value):
    rows = [("w", 0x43, 0, 2), ("W", 0xf1, 1, 4),
            ("w", 0x43, 156, 8), ("R", 0xfd, 1 if value == 200 else 2, 10),
            ("w", 0x43, value, 12), ("w", 0x43, 32, 14),
            ("H", 0x2140, 1, 16)]
    return {"format": "gbb-apu-bus-v2", "source": source,
            "phase_writes_from_reset": True, "history_window_half_clocks": [6, 15],
            "apu_half_hz": 2048000, "events": [dict(kind=k, address=a, value=v, spc_half_clock=t)
                                               for k, a, v, t in rows]}


class Contracts(unittest.TestCase):
    def test_first_transient_is_not_hidden(self):
        result = compare(capture("gbb", 200), capture("reference", 244))
        self.assertEqual(result["matching_phase_write_values"], 2)
        diff = result["first_differing_value"]
        self.assertEqual(diff["ordinal_zero_based"], 2)
        self.assertEqual(diff["gbb"]["next_phase"], diff["reference"]["next_phase"])
        self.assertEqual(diff["reference"]["timer_reads_between_phase_writes"][0]["ticks"], 2)

    def test_missing_window_does_not_mean_no_reads(self):
        data = capture("gbb", 200)
        del data["history_window_half_clocks"]
        result = compare(data, capture("reference", 244))
        self.assertIsNone(result["first_differing_value"]["gbb"]["timer_reads_between_phase_writes"])

    def test_no_clock_alignment(self):
        data = capture("reference", 244)
        data["history_window_half_clocks"] = [106, 115]
        for e in data["events"]:
            e["spc_half_clock"] += 100
        result = compare(capture("gbb", 200), data)
        self.assertEqual(result["matching_phase_write_values"], 2)
        self.assertEqual(result["first_differing_value"]["reference"]["half_clock"], 112)

    def test_require_provenance(self):
        for value in (None, False, 1, "true"):
            data = capture("gbb", 200)
            data["phase_writes_from_reset"] = value
            with self.assertRaisesRegex(ValueError, "marked"):
                sequence(data)

    def test_missing_command_or_enable(self):
        for kind in ("H", "W"):
            data = capture("gbb", 200)
            data["events"] = [e for e in data["events"] if e["kind"] != kind]
            with self.assertRaises(ValueError):
                sequence(data)

    def test_count_difference_is_reported(self):
        data = capture("reference", 200)
        data["events"].pop(5)
        result = compare(capture("gbb", 200), data)
        self.assertIsNone(result["first_differing_value"])
        self.assertEqual(result["pre_command_write_counts"], [4, 3])

    def test_default_divider_straddles_boundary(self):
        gbb = default_timer_interval(4467786, 16, 6916982, 6921214)
        ref = default_timer_interval(4609344, 16, 7058556, 7062788)
        self.assertEqual(gbb["expected_ticks"], 1)
        self.assertEqual(ref["expected_ticks"], 2)
        self.assertEqual(gbb["read_half_since_tick"], 4094)
        self.assertEqual(ref["read_half_since_tick"], 4)

    def test_enable_on_pulse_and_zero_target(self):
        model = default_timer_interval(256, 1, 256, 512)
        self.assertEqual(model["first_output_half_clock"], 512)
        self.assertEqual(model["expected_ticks"], 1)
        self.assertEqual(default_timer_interval(0, 0, 0, 65536)["expected_ticks"], 1)

    def test_nondefault_timer_configuration_is_not_assumed(self):
        data = capture("gbb", 200)
        data["events"].insert(1, dict(kind="W", address=0xfa, value=1, spc_half_clock=3))
        self.assertIsNotNone(timer_model(data, 8, 10))
        data["events"].insert(3, dict(kind="W", address=0xfa, value=2, spc_half_clock=6))
        self.assertIsNone(timer_model(data, 8, 10))
        data["events"].pop(3)
        data["events"].insert(3, dict(kind="W", address=0xf0, value=10, spc_half_clock=6))
        self.assertIsNone(timer_model(data, 8, 10))


if __name__ == "__main__":
    unittest.main()
