#!/usr/bin/env python3
"""ROM-free contracts for scripted libretro audio capture and event comparison."""

from array import array
from pathlib import Path
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from capture_sgb_libretro_audio import (BUTTON_IDS, decode_video_row,
                                        load_input_script, schedule_input)
from compare_sgb_title_audio import compare, correlation, scene_agreement


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
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0,
                         gbb_scene=gbb_scene, reference_scene=reference_scene)
        assert "Same-scene checkpoint: 100.0%" in report
        assert "25ms RMS-envelope correlation at nominal frame=" in report
        assert "best within search range=" in report
        assert "does not establish waveform fidelity" in report
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
