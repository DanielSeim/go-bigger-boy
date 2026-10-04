import importlib.util
from pathlib import Path
import unittest
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location("gate", Path(__file__).resolve().parents[1] / "scripts/check_sgb_frontend_playback.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def trace(driver="wasapi", interval=16640, resets=0):
    lines = ["build=release iterator_debug=0", f"firmware_qualification version=1 model=sgb2 frames=5000 audio_available=1 audio_enabled=1 audio_driver={driver} audio_empty_queue_events=3 audio_latency_resets={resets}"]
    lines += [f"firmware_frame index={n} elapsed_us={n*interval} work_us=11000" for n in range(1, 5001)]
    return "\n".join(lines + ["firmware_qualification_complete frames=5000"])


class GateTests(unittest.TestCase):
    def test_good(self):
        result = gate.evaluate(trace())
        self.assertTrue(result["passed"])
        self.assertEqual(result["empty_input_queue_observations"], 3)

    def test_dummy(self):
        self.assertFalse(gate.evaluate(trace("dummy"))["passed"])
        self.assertTrue(gate.evaluate(trace("dummy"), allow_dummy=True)["passed"])

    def test_slow_and_reset(self):
        self.assertFalse(gate.evaluate(trace(interval=22000))["passed"])
        self.assertFalse(gate.evaluate(trace(resets=1))["passed"])
        self.assertFalse(gate.evaluate(trace().replace("audio_enabled=1", "audio_enabled=0"))["passed"])

    def test_truncated_duplicate_invalid(self):
        original = trace()
        for text in (original.rsplit("\n", 1)[0], original + "\n" + original,
                     original.replace("index=50 ", "index=49 "),
                     original.replace("elapsed_us=832000 ", "elapsed_us=1 "),
                     original.replace("work_us=11000", "work_us=-1"),
                     original.replace("version=1", "version=2")):
            with self.assertRaises(ValueError):
                gate.evaluate(text)
        with self.assertRaises(ValueError):
            gate.evaluate(original.replace("build=release", "build=debug"))

    def test_duration(self):
        with self.assertRaises(ValueError):
            gate.evaluate(trace(), minimum_seconds=90)
        with self.assertRaises(ValueError):
            gate.evaluate(trace()+"\nframe_timing frame=60 core_steps=0 frontend_fps=60")

    def test_tail_stall_not_hidden_by_average(self):
        lines = trace().splitlines()
        for n, line in enumerate(lines):
            if line.startswith("firmware_frame "):
                values = gate.fields(line)
                if int(values["index"]) >= 2500:
                    lines[n] = line.replace("elapsed_us=" + values["elapsed_us"],
                        "elapsed_us=" + str(int(values["elapsed_us"]) + 200000))
        result = gate.evaluate("\n".join(lines))
        self.assertGreater(result["fps"], 59.5)
        self.assertFalse(result["passed"])

    def test_stage_diagnostics(self):
        text = trace().replace("work_us=11000", "work_us=11000 events_us=20 emulation_us=8000 present_us=1000")
        self.assertEqual(gate.evaluate(text)["worst_frame"]["present_us"], 1000)
        with self.assertRaises(ValueError):
            gate.evaluate(text.replace("present_us=1000", "present_us=20000"))

    def test_cli_requires_both_models(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory)/"sgb.log", Path(directory)/"sgb2.log"
            first.write_text(trace().replace("model=sgb2", "model=sgb"))
            second.write_text(trace())
            command = [sys.executable, str(Path(gate.__file__)), str(first)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            self.assertEqual(subprocess.run(command+[str(second)], capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run(command+[str(second), "--warmup-seconds", "nan"], capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
