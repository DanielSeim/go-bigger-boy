#!/usr/bin/env python3
"""Audio gates and CPU-executed logo-free music/effect fixtures; no Nintendo inputs."""
from array import array
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import wave

PROBE = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import compare_dmg_boot_audio as audio


def tone(frequency=512, gain=4000, phase=0, frames=48_000, right_phase=0):
    pcm = array("h")
    for i in range(frames):
        pcm.extend((round(gain * math.sin(2 * math.pi * frequency * i / 48_000 + phase)),
                    round(gain * math.sin(2 * math.pi * frequency * i / 48_000 + phase + right_phase))))
    return pcm


def homebrew(preserve_apu=False):
    rom = bytearray(32768)
    rom[0x100:0x103] = bytes([0xC3, 0x50, 0x01])
    code = bytearray([0xF3, 0x31, 0xFE, 0xFF])
    def write(address, value):
        code.extend([0x3E, value, 0xE0, address])
    def wait(count=37449):
        code.extend([0x01, count & 255, count >> 8, 0x0B, 0x78, 0xB1, 0x20, 0xFB])
    if not preserve_apu:
        write(0x26, 0)
        write(0x04, 0)
    write(0x26, 0x80)
    write(0x24, 0x77)
    # Channel 1: 512 Hz quarter-second, then 1024 Hz; left-only.
    write(0x25, 0x10)
    write(0x11, 0x80)
    write(0x12, 0xF0)
    write(0x13, 0)
    write(0x14, 0x87)
    wait()
    write(0x13, 0x80)
    write(0x14, 0x87)
    wait()
    write(0x12, 0)
    # Channel 2: right-only length-limited 512 Hz effect.
    write(0x25, 0x02)
    write(0x16, 0xA0)
    write(0x17, 0xF2)
    write(0x18, 0)
    write(0x19, 0xC7)
    wait()
    # Wave: original alternating ramp pattern, not cartridge sample data.
    write(0x1A, 0)
    for i in range(16):
        write(0x30 + i, (i << 4) | (15 - i))
    write(0x25, 0x44)
    write(0x1A, 0x80)
    write(0x1C, 0x20)
    write(0x1D, 0x80)
    write(0x1E, 0x87)
    wait()
    write(0x1A, 0)
    # Noise: stereo, decaying envelope; then an explicit quiet gap.
    write(0x25, 0x88)
    write(0x20, 0x20)
    write(0x21, 0xF1)
    write(0x22, 0x35)
    write(0x23, 0xC0)
    wait()
    write(0x21, 0)
    write(0x25, 0)
    wait()
    code.extend([0x18, 0xFE])
    rom[0x150:0x150 + len(code)] = code
    rom[0x14D] = (-sum(rom[0x134:0x14D]) - 25) & 255
    return rom


class MetricTests(unittest.TestCase):
    def test_phase_shift_and_antiphase_stereo_pass(self):
        a = tone(phase=.9, right_phase=math.pi)
        b = tone(right_phase=math.pi)
        result = audio.compare_audio(a, b, [], [])
        self.assertEqual(result["status"], "pass", result)
        self.assertGreater(result["metrics"]["mean_spectral_cosine"], .999)

    def test_pitch_change_fails(self):
        result = audio.compare_audio(tone(frequency=550), tone(), [], [])
        self.assertEqual(result["status"], "fail")
        self.assertIn("mean_centroid_relative_error", result["failures"])

    def test_gain_change_fails_without_normalization(self):
        result = audio.compare_audio(tone(gain=6000), tone(), [], [])
        self.assertIn("rms_gain_delta_db", result["failures"])

    def test_stereo_balance_change_cannot_hide_in_combined_energy(self):
        original = tone()
        changed = array("h", original)
        for i in range(0, len(changed), 2):
            changed[i] = 0
            changed[i + 1] = round(original[i + 1] * math.sqrt(2))
        self.assertIn("channel_gain_delta_db", audio.compare_audio(changed, original, [], [])["failures"])

    def test_gap_and_late_onset_fail(self):
        original = tone()
        missing = array("h", original)
        missing[24_000:36_000] = array("h", [0]) * 12_000
        result = audio.compare_audio(missing, original, [], [])
        self.assertIn("longest_gap_delta_ms", result["failures"])
        late = array("h", [0]) * 12_000 + original[:-12_000]
        self.assertIn("onset_delta_ms", audio.compare_audio(late, original, [], [])["failures"])

    def test_clipping_and_register_pitch_errors_fail(self):
        clipped = tone(gain=6000)
        clipped[100:300] = array("h", [32767]) * 200
        self.assertIn("additional_clipping_fraction", audio.compare_audio(clipped, tone(gain=6000), [], [])["failures"])
        a, b = [[100, 0xFF13, 0]], [[100, 0xFF13, 1]]
        self.assertIn("sound_register_commands", audio.compare_audio(tone(), tone(), a, b)["failures"])

    def test_event_timing_is_not_aligned_away(self):
        a, b = [[10_000, 0xFF14, 0x87]], [[0, 0xFF14, 0x87]]
        self.assertIn("max_event_timing_delta_ms", audio.compare_audio(tone(), tone(), a, b)["failures"])

    def test_silence_is_incomplete_not_success(self):
        quiet = array("h", [5]) * 96_000
        result = audio.compare_audio(quiet, quiet, [], [])
        self.assertEqual(result["status"], "incomplete")
        self.assertFalse(result["coverage_sufficient"])

    def test_unobserved_transient_tails_are_not_false_spectral_failures_or_coverage(self):
        pcm = array("h", [0]) * 96_000
        for start in range(0, len(pcm), 4800):
            for i in range(start + 4768, start + 4800):
                pcm[i] = 4000 if i % 4 < 2 else -4000
        result = audio.compare_audio(pcm, pcm, [], [])
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(result["metrics"]["compared_spectral_windows"], 0)

    def test_truncated_and_wrong_duration_captures_fail(self):
        with self.assertRaises(ValueError):
            audio.compare_audio(tone()[:-1], tone(), [], [])
        with self.assertRaises(ValueError):
            audio.compare_audio(tone(frames=46_000), tone(), [], [])


class ExecutionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="gbb-dmg-audio-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.rom = self.root / "original-music.gb"
        self.rom.write_bytes(homebrew())
        # An opaque *GBB* reference fixture, never a proprietary boot image.
        header = (ROOT / "firmware/gameboy/dmg_boot_image.hpp").read_text()
        self.boot = self.root / "gbb-reference.bin"
        self.boot.write_bytes(bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", header)))

    def capture(self, kind, *options):
        directory = self.root / kind
        command = [PROBE, str(self.rom), "--audio-directory", str(directory),
                   "--run-cycles", str(2 * audio.CLOCK), *options]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshot = json.loads(result.stdout)
        return directory, snapshot, audio.read_pcm(directory / "followup.pcm", snapshot)

    def test_cpu_music_and_effects_capture_without_changing_execution(self):
        path, snapshot, pcm = self.capture("first")
        _, reference, reference_pcm = self.capture("second", "--boot-rom", str(self.boot))
        result = audio.compare_audio(pcm, reference_pcm, snapshot["apu_writes"], reference["apu_writes"])
        self.assertEqual(result["status"], "pass", result)
        self.assertEqual(pcm, reference_pcm)
        addresses = {e[1] for e in snapshot["apu_writes"]}
        self.assertTrue({0xFF14, 0xFF19, 0xFF1E, 0xFF23, 0xFF30, 0xFF3F}.issubset(addresses))
        self.assertGreater(result["replacement"]["longest_gap_ms"], 0)
        plain = subprocess.run([PROBE, str(self.rom), "--run-cycles", str(2 * audio.CLOCK)],
                               capture_output=True, text=True, check=True, timeout=60)
        uninstrumented = json.loads(plain.stdout)
        self.assertEqual(uninstrumented["handoff"], snapshot["handoff"])
        self.assertEqual(uninstrumented["followup"], snapshot["followup"])
        self.assertEqual(uninstrumented["audio"], snapshot["audio"])
        before = (path / "followup.pcm").read_bytes()
        failed = subprocess.run([PROBE, str(self.rom), "--audio-directory", str(path)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(before, (path / "followup.pcm").read_bytes())

    def test_start_pulse_has_bounded_cycle_provenance(self):
        _, snapshot, _ = self.capture("input", "--press-start-cycle", "100")
        pressed, released = snapshot["start_input_events"]
        self.assertEqual(pressed[1], 1)
        self.assertEqual(released[1], 0)
        self.assertLessEqual(abs(pressed[0] - 100), 24)
        self.assertLessEqual(abs(released[0] - 70_324), 24)

    def test_driver_that_keeps_apu_powered_has_correct_notes_and_panning(self):
        self.rom.write_bytes(homebrew(preserve_apu=True))
        _, snapshot, pcm = self.capture("inherited")
        self.assertNotIn([0xFF26, 0], [e[1:] for e in snapshot["apu_writes"]])
        for second, frequency in ((.1, 512), (.35, 1024)):
            start = round(second * audio.RATE)
            left = pcm[start * 2:(start + audio.FFT_SIZE) * 2:2]
            right = pcm[start * 2 + 1:(start + audio.FFT_SIZE) * 2:2]
            powers = audio.fft(left)
            peak = max(range(1, len(powers)), key=powers.__getitem__)
            self.assertLess(abs(peak * audio.RATE / audio.FFT_SIZE - frequency), 24)
            self.assertLessEqual(max(abs(x) for x in right), 8)
            self.assertGreater(max(abs(x) for x in left), 1000)
        # CH2 is right-only, and its enabled 32-step length expires after 125 ms.
        start = round(.58 * audio.RATE) * 2
        segment = pcm[start:start + 2048]
        self.assertLessEqual(max(abs(x) for x in segment[::2]), 8)
        self.assertGreater(max(abs(x) for x in segment[1::2]), 1000)
        start = round(.7 * audio.RATE) * 2
        self.assertLessEqual(max(abs(x) for x in pcm[start:start + 2048]), 8)

    def test_pcm_metadata_detects_tampering(self):
        path, snapshot, _ = self.capture("tamper")
        pcm = path / "followup.pcm"
        payload = bytearray(pcm.read_bytes())
        payload[0] ^= 1
        pcm.write_bytes(payload)
        with self.assertRaisesRegex(ValueError, "hash"):
            audio.read_pcm(pcm, snapshot)

    def test_cli_report_contains_no_pcm_or_wave_ram_and_no_overwrite(self):
        output = self.root / "summary.json"
        command = [sys.executable, str(ROOT / "scripts/compare_dmg_boot_audio.py"),
                   "--probe", PROBE, "--reference-boot", str(self.boot), "--rom", str(self.rom),
                   "--seconds", "2", "--output", str(output),
                   "--capture-directory", str(self.root / "private-captures")]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "pass")
        title = report["titles"][0]
        self.assertNotIn("apu_writes", title)
        self.assertNotIn("vram", title)
        self.assertIn("sha256", title["pcm"]["reference"])
        directory = self.root / "private-captures/0-reference"
        with wave.open(str(directory / "followup.wav"), "rb") as wav:
            self.assertEqual((wav.getframerate(), wav.getnchannels(), wav.getsampwidth()), (48_000, 2, 2))
            self.assertEqual(wav.readframes(wav.getnframes()), (directory / "followup.pcm").read_bytes())
        before = output.read_bytes()
        failed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(output.read_bytes(), before)
        # Even a different report path cannot overwrite the private capture bundle.
        command[command.index(str(output))] = str(self.root / "other-report.json")
        failed = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(failed.returncode, 2)
        self.assertFalse((self.root / "other-report.json").exists())


if __name__ == "__main__":
    unittest.main()
