#!/usr/bin/env python3
"""ROM-free regressions for native-cycle checkpoint and write comparisons."""
import csv
import hashlib
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from compare_sgb_native_writes import native_writes, pcm_difference, compare as compare_writes
from compare_sgb_sound_ram import compare as compare_states


def writes(reference=False):
    result = []
    for cycle, address, value in ((32005, 76, 4), (33526, 12, 4), (33551, 28, 4),
                                  (862392, 34, 157), (862428, 35, 5)):
        row = {"kind": "D", "sample": (cycle + 4) // 32,
               "address": address, "value": value}
        row.update({"clock64": cycle % 64} if reference else {"cycle": cycle})
        result.append(row)
    if not reference:
        result.append({"kind": "Q", "sample": 1000, "value": 4, "address": 5})
    return result


class NativeComparisons(unittest.TestCase):
    def test_phase_rollover_and_equal_acceptance(self):
        ours, reference = native_writes(writes()), native_writes(writes(True), True)
        self.assertEqual(ours, reference)
        self.assertEqual(ours[1], (1521, 12, 4))
        self.assertEqual(ours[-1], (830423, 35, 5))

    def test_upstream_timing_is_not_value_corruption(self):
        ref = writes(True)
        for row in ref[1:3]:
            # Shift exactly 64 native outputs: preserve every written value.
            row["sample"] += 64
        report = compare_writes(writes(), ref)
        self.assertIn("timing only; reference-GBB=+2048 clocks", report)
        self.assertIn("$22: 1 accepted writes match exactly", report)
        ref[1]["value"] = 5
        self.assertIn("different values", compare_writes(writes(), ref))

    def test_no_retiming_to_hide_error(self):
        data = writes(True)
        data[1]["clock64"] = (data[1]["clock64"] + 1) % 64
        self.assertNotEqual(native_writes(writes()), native_writes(data, True))

    def test_bad_write_clocks(self):
        for phase in (None, True, 64, -1):
            with self.subTest(phase=phase), self.assertRaises(ValueError):
                data = writes(True)
                data[0]["clock64"] = phase
                native_writes(data, True)
        data = writes()
        data[-1]["address"] += 1
        with self.assertRaises(ValueError):
            native_writes(data)
        data = writes(True)
        data[0]["value"] = 8
        with self.assertRaisesRegex(ValueError, "KON masks differ"):
            compare_writes(writes(), data)
        data = writes()
        data[2]["cycle"] = 1
        with self.assertRaises(ValueError):
            native_writes(data)

    def test_first_native_output_and_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = [root / name for name in ("gbb.wav", "reference.wav")]
            payload = struct.pack("<hh", 100, -100) * 6000
            for path, rate in zip(paths, (32000, 32040)):
                with wave.open(str(path), "wb") as output:
                    output.setnchannels(2)
                    output.setsampwidth(2)
                    output.setframerate(rate)
                    output.writeframes(payload)
            metadata = {"sample_rate": 32040, "sample_count": 6000,
                        "pcm_sha256": hashlib.sha256(payload).hexdigest()}
            self.assertIn("match exactly", pcm_difference(*paths, writes(), writes(True), metadata))
            changed = bytearray(payload)
            changed[(1000+48)*4:(1000+48)*4+4] = struct.pack("<hh", 101, -100)
            with wave.open(str(paths[1]), "wb") as output:
                output.setnchannels(2)
                output.setsampwidth(2)
                output.setframerate(32040)
                output.writeframes(changed)
            with self.assertRaisesRegex(ValueError, "not bound"):
                pcm_difference(*paths, writes(), writes(True), metadata)
            metadata["pcm_sha256"] = hashlib.sha256(changed).hexdigest()
            self.assertIn("buffer index 48 (output 49 after KON)",
                          pcm_difference(*paths, writes(), writes(True), metadata))
    def fixtures(self, root, native=True):
        gbb, ref = root / "gbb.csv", root / "reference.json"
        rows = [["D", 0, 32005, 1000, 76, 4], ["Q", 0, 32005, 1000, 5, 4],
                ["R", 0, 1, 23400, 32, 0], ["D", 0, 863493, 26984, 76, 4],
                ["Q", 0, 863493, 26984, 5, 4]]
        events = [{"kind": "dsp", "address": 76, "value": 4, "dsp_sample": s,
                   "dsp_clock64": 5} for s in (2000, 27984)]
        events.append({"kind": "ram", "address": 32, "value": 0, "dsp_sample": 24400})
        for index, offset in enumerate((24000, 26240, 26624)):
            for field in range(25):
                value = 13749 if index == 1 and field == 8 else 0
                rows.append(["V", 0, 1, 1000 + offset, field, value])
                events.append({"kind": "state", "address": field, "value": value,
                               "dsp_sample": 2000 + offset, "dsp_phase": 28})
        with gbb.open("w", newline="") as output:
            writer = csv.writer(output)
            writer.writerow(["kind", "master_clock", "spc_cycle", "pcm_sample", "address", "value"])
            writer.writerows(rows)
        data = {"format": "gbb-libretro-audio-timeline-v1", "audio_source": "snes-only",
                "native_cycle_checkpoints": native, "post_audible_sound_writes": events}
        ref.write_text(json.dumps(data))
        return gbb, ref, data

    def test_equal_native_state_not_equal_wall_time(self):
        with tempfile.TemporaryDirectory() as directory:
            gbb, ref, data = self.fixtures(Path(directory))
            report = compare_states(gbb, ref, require_cycle_checkpoints=True,
                                    equal_native_checkpoints=True)
            with self.assertRaisesRegex(ValueError, "not wall-time labels"):
                compare_states(gbb, ref)
            self.assertIn("first KON +26240 native outputs", report)
            self.assertIn("Second-KON elapsed outputs: GBB=640, reference=640", report)
            self.assertIn("interpolation positions differ on voices none", report)
            # The former reference checkpoint was 33 outputs later.
            for event in data["post_audible_sound_writes"]:
                if event["kind"] == "state" and event["dsp_sample"] == 28240:
                    event["dsp_sample"] += 33
            ref.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "exact requested output count"):
                compare_states(gbb, ref, equal_native_checkpoints=True)

    def test_wall_capture_cannot_claim_native_checkpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            gbb, ref, _ = self.fixtures(Path(directory), native=False)
            with self.assertRaisesRegex(ValueError, "explicitly captured"):
                compare_states(gbb, ref, equal_native_checkpoints=True)


if __name__ == "__main__":
    unittest.main()
