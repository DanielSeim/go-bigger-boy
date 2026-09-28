#!/usr/bin/env python3
"""ROM-free contracts for scripted libretro audio capture and event comparison."""

from array import array
from pathlib import Path
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from capture_sgb_libretro_audio import BUTTON_IDS, load_input_script, schedule_input
from compare_sgb_title_audio import compare, correlation


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


def main() -> None:
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
        write_wave(gbb, 32000, 1.15)
        write_wave(reference, 48000, 1.15)
        report = compare(gbb, reference, 32000, 60, 0.1, 2.0)
        assert "25ms RMS-envelope correlation at nominal frame=" in report
        assert "best within search range=" in report
        assert "does not establish waveform fidelity" in report
        print("script mapping and guarded audio comparison pass")


if __name__ == "__main__":
    main()
