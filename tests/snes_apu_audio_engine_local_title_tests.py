#!/usr/bin/env python3
"""Private-ROM-only core APU parity with the established native title path.

This is integration parity, not a new independent hardware accuracy claim.
No ROM, firmware, state or PCM is copied into the repository.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import wave
from snes_apu_audio_engine_pcm_tests import validate_benchmark

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("roms", type=Path)
    parser.add_argument("script", type=Path)
    args = parser.parse_args()
    game = args.roms / "Donkey Kong (JU) (V1.1) [S][!].gb"
    if hashlib.sha256(game.read_bytes()).hexdigest() != \
            "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4":
        raise AssertionError("unexpected local Donkey Kong ROM")
    with tempfile.TemporaryDirectory(prefix="gbb-core-apu-title-") as directory:
        for model in ("sgb1", "sgb2"):
            files = []
            for mode in ("--fractional-apu-sync", "--core-apu-engine"):
                output = Path(directory) / f"{model}{mode}.wav"
                command = [str(args.trace), str(args.roms / f"{model}.program.rom"),
                           str(args.roms / "spc700.rom"), f"--sync-gb-{model}", str(game),
                           str(args.roms / ("sgb.boot.rom" if model == "sgb1" else "sgb2.boot.rom")),
                           "--input-script", str(args.script), "--native-gb-input",
                           "--instruction-limit", "60000000",
                           "--ppu-dma-timing", "--host-bus-timing", mode, "--pcm-output", str(output)]
                if mode == "--core-apu-engine":
                    benchmark = Path(directory) / f"{model}-performance.json"
                    command.extend(["--core-apu-state-roundtrip", "--core-apu-benchmark-output", str(benchmark)])
                result = subprocess.run(command, capture_output=True, text=True, timeout=240)
                if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in result.stderr:
                    raise AssertionError(result.stdout[-2000:] + result.stderr[-2000:])
                if mode == "--core-apu-engine":
                    trials = validate_benchmark(benchmark, model)
                    print(f"{model}: isolated firmware/music APU realtime ratios " +
                          ", ".join(f"{t['realtime_ratio']:.2f}x" for t in trials), flush=True)
                files.append(output.read_bytes())
                with wave.open(str(output), "rb") as wav:
                    if wav.getframerate() != 32000 or wav.getnchannels() != 2 or wav.getsampwidth() != 2:
                        raise AssertionError("bad native PCM format")
                    if not any(wav.readframes(wav.getnframes())):
                        raise AssertionError("title audio was silent")
            if files[0] != files[1]:
                raise AssertionError(f"{model}: reusable core scheduler altered native title PCM")
            pinned = ("1b4cc438b6aadcf0916912698fede5396386c6f2656b3c5853c71ca971260311" if model == "sgb1" else
                      "d9211313e26aa4c200d40df114b711a564ecd7418063747579bedbb0c21d096e")
            if hashlib.sha256(files[1]).hexdigest() != pinned:
                raise AssertionError(f"{model}: native WAV baseline changed in both paths")
            print(f"{model}: title gameplay native WAV parity {hashlib.sha256(files[1]).hexdigest()}", flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
