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
        # Phase-correct baselines pin this bounded implementation, not independent
        # hardware fidelity. Scalar playback and restore must preserve its PCM.
        for model, rate, baseline, combined_baseline in (
                ("sgb1", 48000, "56aa9bc74cdbe70711c43eadbdf809f39e6c63c207a83f6f08a1f0f595e3c7e3",
                 "bfb70ddb9b22ed3c4618cf9e64fd4b399ffd6cf9e717c1ffdcac4898b9d6824d"),
                ("sgb2", 44100, "8d2b85cfceb9b744e03946794da7ba0705a836436b0d444d7bed2907e8fce8ea",
                 "10f1d1c7ef4aa1e1f460c74804f7bdf8b98e27eda17aa7a79a6d01e900ed9a5f")):
            reports, outputs = [], []
            for mode in ("native", "combined", "restored", "scalar"):
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
                if mode == "scalar":
                    command.extend(["--scalar-apu", "--scalar-spc", "--scalar-dsp", "--callback-dsp"])
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
            if any(outputs[1] != outputs[i] or any(reports[1][key] != reports[i][key] for key in
                    ("samples", "nonzero", "gb_samples", "clipped_samples", "gb_state_hash"))
                    for i in (2, 3)):
                raise AssertionError("restoration/consumer partitions/scalar playback changed combined output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
