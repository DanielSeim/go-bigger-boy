#!/usr/bin/env python3
"""Original bus fixtures: distinguish timer timing, tick values and phase state."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_sgb_timer_polls import compare, observations


def capture(source, phase):
    data = {"format": "gbb-apu-bus-v2", "source": source,
            "apu_half_hz": 2048000 if source == "gbb" else 2050560,
            "events": []}
    def add(kind, address, value, half):
        data["events"].append(dict(kind=kind, address=address, value=value,
                                   spc_half_clock=half))
    add("w", 0x43, phase, 90)
    add("K", 0xf3, 4, 100)
    add("R", 0xfd, 2, 658)
    add("r", 0x43, phase, 702)
    phase = (phase + 88) & 255
    add("w", 0x43, phase, 710)
    add("R", 0xfd, 1, 2976)
    add("r", 0x43, phase, 3020)
    add("w", 0x43, (phase + 44) & 255, 3028)
    return data


class Contracts(unittest.TestCase):
    def test_same_timer_different_carry(self):
        result = compare(capture("gbb", 164), capture("reference", 104))
        self.assertEqual(result["matched_timer_poll_prefix"], 2)
        self.assertEqual(result["last_pre_kon_phase"], [164, 104])
        second = result["same_poll_different_phase"][1]
        self.assertTrue(second["gbb"]["wrapped"])
        self.assertFalse(second["reference"]["wrapped"])
        self.assertEqual(second["gbb"]["inferred_increment"], 44)
        self.assertIsNone(result["first_poll_difference"])

    def test_offset_is_not_hidden(self):
        gbb, ref = capture("gbb", 164), capture("reference", 104)
        ref["events"][5]["spc_half_clock"] -= 16
        result = compare(gbb, ref)
        self.assertEqual(result["matched_timer_poll_prefix"], 1)
        self.assertEqual(len(result["same_poll_different_phase"]), 1)
        self.assertNotEqual(result["first_poll_difference"]["gbb"]["half_clocks_after_kon"],
                            result["first_poll_difference"]["reference"]["half_clocks_after_kon"])
        self.assertEqual(len(result["positive_poll_state_pairs"]), 2)

    def test_missing_state_is_not_inferred(self):
        gbb = capture("gbb", 164)
        gbb["events"] = [e for e in gbb["events"] if e["kind"] not in "rw"]
        result = compare(gbb, capture("reference", 104))
        self.assertEqual(result["same_poll_different_phase"], [])
        self.assertEqual(result["positive_poll_state_pairs"], [])

    def test_stop_at_next_poll(self):
        data = capture("gbb", 164)
        data["events"].pop(4)
        first = observations(data)["polls"][0]
        self.assertNotIn("phase_after", first)

    def test_missing_profile_and_anchor(self):
        for change in (lambda d: d.update(format="gbb-apu-bus-v1"),
                       lambda d: d.update(events=[e for e in d["events"] if e["kind"] != "K"]),
                       lambda d: d.update(events=[e for e in d["events"] if e["kind"] != "R"])):
            data = capture("gbb", 164)
            change(data)
            with self.assertRaises(ValueError):
                observations(data)

    def test_no_common_prefix(self):
        other = copy.deepcopy(capture("reference", 104))
        other["events"][2]["value"] = 1
        with self.assertRaisesRegex(ValueError, "no common"):
            compare(capture("gbb", 164), other)

    def test_truncated_capture(self):
        other = capture("reference", 104)
        other["events"] = other["events"][:5]
        result = compare(capture("gbb", 164), other)
        self.assertIsNone(result["first_poll_difference"]["reference"])

    def test_phase_read_precedes_command_by_event_order(self):
        data = capture("gbb", 164)
        data["events"][:0] = [dict(kind="r", address=0x43, value=80, spc_half_clock=10),
                              dict(kind="H", address=0x2140, value=1, spc_half_clock=20)]
        self.assertEqual(observations(data)["first_pre_command_phase_read"],
                         {"value": 80, "half_clocks_after_kon": -90})
        data["events"][:2] = list(reversed(data["events"][:2]))
        self.assertIsNone(observations(data)["first_pre_command_phase_read"])


if __name__ == "__main__":
    unittest.main()
