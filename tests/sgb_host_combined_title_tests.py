#!/usr/bin/env python3
"""Private combined-audio replay; no firmware, captures or state is distributed."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
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
        raise AssertionError("unexpected private game image")
    with tempfile.TemporaryDirectory(prefix="gbb-combined-host-title-") as directory:
        # Combined hashes pin the original uncached prototype, not independent
        # hardware fidelity. They prevent optimization from changing its PCM.
        for model, rate, baseline, combined_baseline in (
                ("sgb1", 48000, "553c8992a2de7dbc86eac6de3132000299f2fcb7e73fbe8ef0d03f9c8ec6856f",
                 "53d94251ed69f1edbd8875088b6670fdf97633c9474fde8c18a45081faacdd06"),
                ("sgb2", 44100, "d430fa49f199df87fc20b6465cba0fff0e89daf6b446a9a07159b90b188e4bc9",
                 "8854bc396db0a99828d2e34fefba362498d2cab21039b5bb0e2bbb795fdc3071")):
            reports, outputs = [], []
            for mode in ("native", "combined", "restored"):
                prefix = Path(directory) / f"{model}-{mode}"
                wav, report = prefix.with_suffix(".wav"), prefix.with_suffix(".json")
                command = [str(args.runner), model, str(args.roms / f"{model}.program.rom"),
                           str(args.roms / "spc700.rom"), str(game),
                           str(args.roms / ("sgb.boot.rom" if model == "sgb1" else "sgb2.boot.rom")),
                           str(args.script), str(wav), str(report)]
                if mode != "native":
                    command.extend(["--combined", "--output-hz", str(rate)])
                if mode == "restored":
                    command.extend(["--restore", "--chunk", "257"])
                subprocess.run(command, check=True, timeout=600)
                data = json.loads(report.read_text())
                if data["cpu_seconds"] is not None and (
                        not math.isfinite(data["cpu_seconds"]) or data["cpu_seconds"] <= 0 or
                        not math.isfinite(data["cpu_realtime_ratio"]) or data["cpu_realtime_ratio"] <= 0):
                    raise AssertionError("invalid process CPU timing")
                payload = wav.read_bytes()
                native = mode == "native"
                if (data["steps"] != 60000000 or data["inputs"] != 11 or data["gb_frames"] < 5000 or
                        data["sound_delivered"] != 3 or data["audible_delivered"] != 2 or
                        data["combined"] == native or data["output_hz"] != (32000 if native else rate) or
                        data["clipped_samples"] != 0 or data["nonzero"] <= 0 or
                        not math.isfinite(data["seconds"]) or data["seconds"] <= 0 or
                        not math.isfinite(data["realtime_ratio"]) or data["realtime_ratio"] <= 0 or
                        data["restorations"] != (data["samples"] // 8192 if mode == "restored" else 0) or
                        data["apu_half_clocks"] != data["master_clocks"] * 2048000 // 21477273 or
                        struct.unpack_from("<I", payload, 24)[0] != data["output_hz"] or
                        len(payload) != 44 + data["samples"] * 4):
                    raise AssertionError(f"{model}/{mode}: incomplete combined replay")
                if native:
                    if data["gb_samples"] != 0 or hashlib.sha256(payload).hexdigest() != baseline:
                        raise AssertionError("native SNES audio baseline changed")
                elif (data["gb_samples"] <= 0 or
                      data["samples"] != data["master_clocks"] * rate // 21477273):
                    raise AssertionError("GB capture missing or combined output drifted")
                elif hashlib.sha256(payload).hexdigest() != combined_baseline:
                    raise AssertionError("optimization changed original combined PCM")
                reports.append(data)
                outputs.append(payload)
                print(f"{model}/{mode}: {data['realtime_ratio']:.2f}x whole-host realtime; "
                      f"CPU ratio {data['cpu_realtime_ratio']}; "
                      f"{data['gb_samples']} GB samples, {data['restorations']} restores", flush=True)
            counters = ("steps", "master_clocks", "apu_half_clocks", "snes_samples", "gb_frames",
                        "inputs", "sound_delivered", "audible_delivered")
            if any(reports[0][key] != data[key] for data in reports[1:] for key in counters):
                raise AssertionError("combined presentation changed processor or SOUND timing")
            if outputs[1] != outputs[2] or any(reports[1][key] != reports[2][key] for key in
                    ("samples", "nonzero", "gb_samples", "clipped_samples")):
                raise AssertionError("restoration/consumer partitions changed combined output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
