#!/usr/bin/env python3
"""ROM-free contracts for fractional APU bus capture comparisons."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("apu_bus", ROOT / "scripts/compare_sgb_apu_bus.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
sys.path.insert(0, str(ROOT / "scripts"))
from capture_sgb_libretro_audio import capture as capture_audio


def capture():
    rows = [("H", 0x2140, 1, 10), ("h", 0x2140, 0, 10),
            ("R", 0xf4, 1, 11), ("K", 0xf3, 4, 20), ("K", 0xf3, 4, 24)]
    return {"format": "gbb-apu-bus-v1", "source": "gbb", "master_hz": 21477273,
            "apu_half_hz": 2048000, "events": [
                {"kind": kind, "address": address, "value": value,
                 "master_clock": half * 10, "spc_half_clock": half,
                 "pcm_sample": half // 8, "dsp_clock64": half // 2}
                for kind, address, value, half in rows]}


class Contracts(unittest.TestCase):
    def load(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bus.json"
            path.write_text(json.dumps(data))
            return module.load(path, "gbb")

    def test_exact_domains(self):
        result = module.summarize(self.load(capture()))
        self.assertEqual(result["command_observation_halves"], 1)
        self.assertEqual(result["command_observation_spacing_us"], 1_000_000 / 2048000)
        self.assertEqual(result["midpoint_reads"], 1)
        self.assertEqual(result["key_phases"], [10, 12])

    def test_no_phase_fitting(self):
        data = capture()
        other = copy.deepcopy(data)
        for event in other["events"]:
            event["spc_half_clock"] += 200
            event["master_clock"] += 10000
            event["pcm_sample"] += 50
        self.assertEqual(module.summarize(data), module.summarize(other))
        other["events"][-1]["dsp_clock64"] += 1
        self.assertNotEqual(module.summarize(data)["key_phases"],
                            module.summarize(other)["key_phases"])

    def test_bad_metadata(self):
        for key, value in (("format", "other"), ("source", "reference"),
                           ("master_hz", True), ("apu_half_hz", 0), ("events", [])):
            with self.subTest(key=key), self.assertRaises(ValueError):
                data = capture()
                data[key] = value
                self.load(data)

    def test_bad_events(self):
        for key, value in (("kind", "X"), ("address", 0xf3), ("value", 256),
                           ("dsp_clock64", 64), ("pcm_sample", -1), ("spc_half_clock", True)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                data = capture()
                data["events"][0][key] = value
                self.load(data)

    def test_clock_reversal(self):
        for key in ("master_clock", "spc_half_clock", "pcm_sample"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                data = capture()
                data["events"][-1][key] = 0
                self.load(data)

    def test_missing_handshake(self):
        data = capture()
        data["events"][0]["value"] = 0
        with self.assertRaisesRegex(ValueError, "handshake"):
            module.summarize(self.load(data))
        data = capture()
        data["events"][2]["value"] = 0
        with self.assertRaisesRegex(ValueError, "not observed"):
            module.summarize(self.load(data))

    def test_missing_host_probe(self):
        data = capture()
        data["events"] = data["events"][2:]
        with self.assertRaisesRegex(ValueError, "host accesses"):
            module.summarize(self.load(data))

    def test_capture_preflight(self):
        # Fail before loading a core: these are original dummy files, not ROMs.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = [root / name for name in ("core", "game", "firmware")]
            for path in inputs:
                path.write_bytes(b"dummy")
            bus = root / "bus.json"
            wav = root / "audio.wav"
            with self.assertRaisesRegex(ValueError, "SNES-only"):
                capture_audio(*inputs, root, 1, wav, apu_bus_output=bus)
            with self.assertRaisesRegex(ValueError, "SNES-only"):
                capture_audio(*inputs, root, 1, wav, native_cycle_checkpoints=True)
            with self.assertRaisesRegex(ValueError, "timer polling"):
                capture_audio(*inputs, root, 1, wav, timer_poll_trace=True)
            with self.assertRaisesRegex(ValueError, "boot timeline"):
                capture_audio(*inputs, root, 1, wav, boot_timeline_output=root / "boot.json")
            for entropy in ("None", "invalid"):
                with self.assertRaisesRegex(ValueError, "reference entropy"):
                    capture_audio(*inputs, root, 1, wav, reference_entropy=entropy)
            for window in ((1, 2), (2, 1), (0, 200001), (True, 10)):
                with self.assertRaisesRegex(ValueError, "history window"):
                    capture_audio(*inputs, root, 1, wav, history_window_half=window)
            with self.assertRaisesRegex(ValueError, "paths must differ"):
                capture_audio(*inputs, root, 1, bus, apu_bus_output=bus,
                              timeline_output=root / "timeline.json", require_snes_only_probe=True)
            bus.write_bytes(b"keep")
            with self.assertRaises(FileExistsError):
                capture_audio(*inputs, root, 1, wav, apu_bus_output=bus)
            self.assertEqual(bus.read_bytes(), b"keep")

    def test_timer_profile_is_explicit(self):
        data = capture()
        read = data["events"][2]
        read["address"] = 0xfd
        with self.assertRaises(ValueError):
            self.load(data)
        data["format"] = "gbb-apu-bus-v2"
        self.load(data)
        with self.assertRaisesRegex(ValueError, "input-port reads"):
            module.summarize(self.load(data))
        read["value"] = 16
        with self.assertRaisesRegex(ValueError, "four-bit"):
            self.load(data)
        read["value"] = 1
        read["kind"] = "r"
        read["address"] = 0x10
        self.load(data)
        read["address"] = 0xf0
        with self.assertRaises(ValueError):
            self.load(data)

    def test_history_metadata(self):
        for key, value in (("phase_writes_from_reset", 1),
                           ("history_window_half_clocks", [0, 200001]),
                           ("history_window_half_clocks", [3, 2]),
                           ("history_window_half_clocks", [True, 2])):
            data = capture()
            data["format"] = "gbb-apu-bus-v2"
            data[key] = value
            with self.assertRaises(ValueError):
                self.load(data)


if __name__ == "__main__":
    unittest.main()
