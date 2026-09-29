#!/usr/bin/env python3
"""ROM-free contracts for scripted libretro audio capture and event comparison."""

from array import array
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from capture_sgb_libretro_audio import (BUTTON_IDS, decode_video_row,
                                        load_input_script, schedule_input)
from compare_sgb_audio_phase import rank_offsets, series_transitions
from compare_sgb_title_audio import (compare, correlation, read_stereo_wav,
                                     reference_frame_window, scene_agreement)


def write_wave(path: Path, rate: int, onset: float) -> None:
    values = array("h")
    for index in range(rate * 5):
        seconds = index / rate
        # A deliberately varied, deterministic envelope after the event.
        phase = seconds - onset
        amplitude = (0 if phase < 0 else
                     int((0.25 + (int(phase * 5) % 4) * 0.13) * 12000))
        value = amplitude if index % 2 == 0 else -amplitude
        values.extend((value, value))
    if sys.byteorder != "little":
        values.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(values.tobytes())


def write_scene(path: Path, shift: int = 0, blank: bool = False) -> None:
    pixels = bytearray()
    for y in range(224):
        for x in range(256):
            color = (0 if blank else (x // 8 + y // 8 + shift) % 4) * 64
            pixels.extend((color, color, color))
    path.write_bytes(b"P6\n256 224\n255\n" + pixels)


def main() -> None:
    native = lambda value, width: value.to_bytes(width, sys.byteorder)
    assert decode_video_row(native(0x7C00, 2) + native(0x03E0, 2), 0) == \
        bytes((255, 0, 0, 0, 255, 0))
    assert decode_video_row(native(0xF800, 2) + native(0x001F, 2), 2) == \
        bytes((255, 0, 0, 0, 0, 255))
    assert decode_video_row(native(0x00FF8040, 4), 1) == bytes((255, 128, 64))
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-audio-tools-") as temporary:
        root = Path(temporary)
        script = root / "input.script"
        script.write_text("GBB SGB input v1\n1000 start\n1004 none\n"
                          "1600 a+right\n1604 none\n2800 b\n", encoding="utf-8")
        events = load_input_script(script)
        assert events == [
            (1000, 1 << BUTTON_IDS["start"]), (1004, 0),
            (1600, (1 << BUTTON_IDS["a"]) | (1 << BUTTON_IDS["right"])),
            (1604, 0), (2800, 1 << BUTTON_IDS["b"]),
        ]
        schedule = schedule_input(events, 60.098812, 228,
                                  [(1600, 253), (2800, 321)])
        assert schedule[0][0] < schedule[2][0] < schedule[4][0]
        assert schedule[2][0] == round(1600 * 60.098812 /
                                       (4194304 / 70224)) + 253
        assert schedule[4][0] == round(2800 * 60.098812 /
                                       (4194304 / 70224)) + 321
        for invalid in ("GBB SGB input v1\n1 a+a\n",
                        "GBB SGB input v1\n2 a\n2 none\n",
                        "wrong header\n1 start\n"):
            script.write_text(invalid, encoding="utf-8")
            try:
                load_input_script(script)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid input script was accepted")
        assert abs(correlation([0, 1, 2], [0, 1, 2]) - 1) < 1e-12
        gbb = root / "gbb.wav"
        reference = root / "reference.wav"
        gbb_scene = root / "gbb.ppm"
        reference_scene = root / "reference.ppm"
        write_scene(gbb_scene)
        write_scene(reference_scene)
        assert scene_agreement(gbb_scene, reference_scene) == 1.0
        write_wave(gbb, 32000, 1.15)
        write_wave(reference, 48000, 1.15)
        _, reference_pcm = read_stereo_wav(reference)
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene)
        assert "Same-scene checkpoint: 100.0%" in report
        assert "25ms RMS-envelope correlation at nominal frame=" in report
        assert "best within search range=" in report
        assert "does not establish waveform fidelity" in report
        phase = root / "phase"
        phase.mkdir()
        for number, shift in ((10, 0), (11, 1), (12, 1), (13, 0)):
            write_scene(phase / f"gbb-{number}.ppm", shift=shift)
        frame_range, transitions, threshold = series_transitions(
            str(phase / "gbb-*.ppm"), 0)
        assert frame_range == (10, 13) and set(transitions) == {11, 13}
        assert threshold > 0
        (phase / "gbb-12.ppm").unlink()
        try:
            series_transitions(str(phase / "gbb-*.ppm"), 0)
        except ValueError as error:
            assert "no missing frames" in str(error)
        else:
            raise AssertionError("gapped phase series accepted")
        ranked = rank_offsets((10, 14), {11: 500, 13: 500},
                              (110, 114), {111: 450, 113: 450}, 102, 3)
        assert ranked[0]["offset"] == 100
        assert ranked[0]["matched"] == 2 and ranked[0]["disagreements"] == 0
        try:
            rank_offsets((10, 14), {11: 500, 13: 500},
                         (110, 114), {111: 450, 113: 450}, 102, 121)
        except ValueError as error:
            assert "offset window" in str(error)
        else:
            raise AssertionError("unbounded phase search accepted")
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_scene_frame=60)
        except ValueError as error:
            assert "requires a reference timeline" in str(error)
        else:
            raise AssertionError("unbound scene frame accepted")
        timeline = root / "reference-timeline.json"
        timeline.write_text(json.dumps({
            "format": "gbb-libretro-audio-timeline-v1",
            "sample_rate": 48000, "sample_count": 240000,
            "pcm_sha256": hashlib.sha256(reference_pcm.tobytes()).hexdigest(),
            "snapshot_sha256": {
                "60": hashlib.sha256(reference_scene.read_bytes()).hexdigest()},
            "run_samples": [[0, 47600], [47600, 48500], [48500, 240000]],
            "video_callbacks": [{"video_frame": 60, "run_index": 1,
                                 "sample_at_callback": 48000}]}), encoding="utf-8")
        assert reference_frame_window(timeline, 60, 48000, reference_pcm) == (47600, 48500)
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene,
                         reference_timeline=timeline, reference_scene_frame=60)
        assert "callback audio range [47600, 48500]" in report
        wrong_scene = root / "wrong-reference.ppm"
        write_scene(wrong_scene, shift=0)
        wrong_scene.write_bytes(wrong_scene.read_bytes() + b"\n")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm, wrong_scene)
        except ValueError as error:
            assert "scene does not match" in str(error)
        else:
            raise AssertionError("unbound reference scene accepted")
        for wrong_frame, wrong_rate in ((61, 48000), (60, 32000)):
            try:
                reference_frame_window(timeline, wrong_frame, wrong_rate, reference_pcm)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid reference timeline accepted")
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene,
                    reference_timeline=timeline, reference_scene_frame=61)
        except ValueError as error:
            assert "differs from predicted" in str(error)
        else:
            raise AssertionError("mismatched reference scene frame accepted")
        broken = json.loads(timeline.read_text(encoding="utf-8"))
        broken["pcm_sha256"] = "0" * 64
        timeline.write_text(json.dumps(broken), encoding="utf-8")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm)
        except ValueError as error:
            assert "PCM content" in str(error)
        else:
            raise AssertionError("wrong reference PCM accepted")
        broken["pcm_sha256"] = hashlib.sha256(reference_pcm.tobytes()).hexdigest()
        broken["run_samples"][1][0] += 1
        timeline.write_text(json.dumps(broken), encoding="utf-8")
        try:
            reference_frame_window(timeline, 60, 48000, reference_pcm)
        except ValueError as error:
            assert "noncontiguous" in str(error)
        else:
            raise AssertionError("noncontiguous reference audio accepted")
        write_scene(reference_scene, shift=1)
        assert scene_agreement(gbb_scene, reference_scene) < 0.65
        try:
            compare(gbb, reference, 32000, 60, 0.1, 2.0,
                    gbb_scene=gbb_scene, reference_scene=reference_scene)
        except ValueError as error:
            assert "scene checkpoint mismatch" in str(error)
        else:
            raise AssertionError("wrong scene was accepted")
        write_scene(reference_scene, blank=True)
        try:
            scene_agreement(gbb_scene, reference_scene)
        except ValueError as error:
            assert "too uniform" in str(error)
        else:
            raise AssertionError("blank scene was accepted")
        print("script mapping and guarded audio comparison pass")


if __name__ == "__main__":
    main()
