#!/usr/bin/env python3
"""Regression tests for the external/SameBoy audio reference converter."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts" / "audio_reference.py"


def write_wav(path: Path, rate: int, frames: int, offset: int = 0) -> None:
    values: list[int] = []
    for index in range(frames):
        values.extend((128 + offset + index, -128 - offset - index))
    payload = b"".join(int(value).to_bytes(2, "little", signed=True)
                       for value in values)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(payload)


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *arguments],
                          check=False, capture_output=True, text=True)


def main() -> int:
    pin = json.loads((ROOT / "tests" / "fixtures" / "audio-external" /
                      "sameboy-reference-pin.json").read_text(encoding="utf-8"))
    if (pin.get("project") != "SameBoy" or pin.get("release") != "v1.0.3" or
            pin.get("commit") != "208ba4afabffab9edde416f2dbb8ae459e34adb8" or
            pin.get("audio_capture_rate_hz") != 96000 or
            pin.get("reference_rate_hz") != 48000):
        print("SameBoy reference pin is incomplete or changed unexpectedly", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="gbb-audio-reference-") as directory:
        root = Path(directory)
        first_wav = root / "first.wav"
        second_wav = root / "second.wav"
        write_wav(first_wav, 96_000, 8)
        write_wav(second_wav, 96_000, 8, 1)
        first = root / "first.txt"
        second = root / "second.txt"
        common = ("--name", "pulse", "--model", "dmg",
                  "--source", "trusted-emulator", "--downsample", "2",
                  "--comparison", "normalized",
                  "--provenance", "SameBoy test commit, DMG, 96 kHz")
        for wav_path, reference_path in ((first_wav, first), (second_wav, second)):
            result = run("convert", str(wav_path), str(reference_path), *common,
                         "--frames", "4")
            if result.returncode != 0:
                print(result.stderr, file=sys.stderr)
                return 1
            text = reference_path.read_text(encoding="utf-8")
            if ("source=trusted-emulator\n" not in text or
                    "comparison=normalized\n" not in text or
                    "sample_rate=48000\n" not in text):
                print("SameBoy conversion did not preserve trusted metadata", file=sys.stderr)
                return 1
        output = root / "aggregate.txt"
        result = run("aggregate", str(first), str(second), str(output),
                     "--source", "trusted-emulator", "--provenance",
                     "SameBoy test aggregate")
        aggregate_text = output.read_text(encoding="utf-8")
        if (result.returncode != 0 or
                "source=trusted-emulator\n" not in aggregate_text or
                "comparison=normalized\n" not in aggregate_text):
            print(result.stderr, file=sys.stderr)
            return 1
        legacy_first = root / "legacy-first.txt"
        legacy_second = root / "legacy-second.txt"
        legacy_first.write_text(first.read_text(encoding="utf-8")
                                .replace("source=trusted-emulator\n", "")
                                .replace("comparison=normalized\n", ""), encoding="utf-8")
        legacy_second.write_text(second.read_text(encoding="utf-8")
                                 .replace("source=trusted-emulator\n", "")
                                 .replace("comparison=normalized\n", ""), encoding="utf-8")
        legacy_output = root / "legacy-normalized.txt"
        result = run("aggregate", str(legacy_first), str(legacy_second),
                     str(legacy_output), "--source", "trusted-emulator",
                     "--comparison", "normalized", "--provenance",
                     "legacy captures upgraded to normalized mode")
        if (result.returncode != 0 or
                "source=trusted-emulator\n" not in legacy_output.read_text(encoding="utf-8") or
                "comparison=normalized\n" not in legacy_output.read_text(encoding="utf-8")):
            print(result.stderr, file=sys.stderr)
            return 1
        rejected = run("convert", str(first_wav), str(root / "rejected.txt"),
                       "--name", "pulse", "--model", "dmg")
        if rejected.returncode == 0:
            print("96 kHz input was accepted without explicit downsampling", file=sys.stderr)
            return 1
        rejected_mode = run("convert", str(first_wav), str(root / "rejected-mode.txt"),
                            "--name", "pulse", "--model", "dmg",
                            "--source", "hardware", "--comparison", "normalized",
                            "--downsample", "2")
        if rejected_mode.returncode == 0:
            print("normalized hardware reference was accepted", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
