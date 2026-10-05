#!/usr/bin/env python3
"""Private title/firmware regression for the original bounded whole SGB host.

No state/ROM/audio is committed. Ratios are measurements, not CI speed gates.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runner", type=Path)
    parser.add_argument("roms", type=Path)
    parser.add_argument("script", type=Path)
    args = parser.parse_args()
    game = args.roms / "Donkey Kong (JU) (V1.1) [S][!].gb"
    if hashlib.sha256(game.read_bytes()).hexdigest() != \
            "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4":
        raise AssertionError("unexpected local game image")
    with tempfile.TemporaryDirectory(prefix="gbb-whole-host-title-") as directory:
        for model, baseline in (
                ("sgb1", "1b4cc438b6aadcf0916912698fede5396386c6f2656b3c5853c71ca971260311"),
                ("sgb2", "d9211313e26aa4c200d40df114b711a564ecd7418063747579bedbb0c21d096e")):
            wavs = []
            reports = []
            for restore in (False, True):
                prefix = Path(directory) / f"{model}-{restore}"
                wav, report = prefix.with_suffix(".wav"), prefix.with_suffix(".json")
                command = [str(args.runner), model, str(args.roms / f"{model}.program.rom"),
                           str(args.roms / "spc700.rom"), str(game),
                           str(args.roms / ("sgb.boot.rom" if model == "sgb1" else "sgb2.boot.rom")),
                           str(args.script), str(wav), str(report)]
                if restore:
                    command.append("--restore")
                subprocess.run(command, check=True, timeout=360)
                data = json.loads(report.read_text())
                if (data["format"] != "gbb-sgb-host-performance-v1" or
                        data.get("gb_reset_profile") != "cold-sgb-v1" or data["model"] != model or
                        data.get("gb_lcd_profile") != "clocked-pixel-v1" or
                        data["steps"] != 60000000 or data["inputs"] != 11 or
                        data["sound_delivered"] != 3 or data["audible_delivered"] != 2 or
                        data["nonzero"] <= 0 or data["gb_frames"] < 5000 or
                        data["restorations"] != (data["samples"] // 8192 if restore else 0) or
                        not math.isfinite(data["seconds"]) or data["seconds"] <= 0 or
                        not math.isfinite(data["realtime_ratio"]) or data["realtime_ratio"] <= 0):
                    raise AssertionError("incomplete whole-host replay")
                if data["apu_half_clocks"] != data["master_clocks"] * 2048000 // 21477273:
                    raise AssertionError("whole-host processors lost clock synchronization")
                payload = wav.read_bytes()
                if hashlib.sha256(payload).hexdigest() != baseline:
                    raise AssertionError(f"{model}: whole host changed established native WAV baseline")
                wavs.append(payload)
                reports.append(data)
                print(f"{model}: {'restored' if restore else 'normal'} complete host "
                      f"{data['realtime_ratio']:.2f}x realtime", flush=True)
            if wavs[0] != wavs[1] or any(reports[0][key] != reports[1][key] for key in
                    ("master_clocks", "apu_half_clocks", "samples", "nonzero", "gb_frames", "inputs")):
                raise AssertionError("whole-host restoration changed execution or PCM")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
