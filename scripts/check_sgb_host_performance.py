#!/usr/bin/env python3
"""Gate bounded SGB host playback headroom, not frontend/device FPS."""
import argparse
import json
import math
from pathlib import Path
import statistics

MASTER_HZ = 21477273


def summarize(data, warmup=10.0, minimum_windows=30):
    if data.get("format") != "gbb-sgb-host-performance-v1" or data.get("windows_complete") is not True:
        raise ValueError("missing or truncated playback windows")
    if data.get("model") not in ("sgb1", "sgb2") or not isinstance(data.get("combined"), bool):
        raise ValueError("invalid model/audio mode")
    if (type(data.get("output_hz")) is not int or
            not 8000 <= data["output_hz"] <= 48000):
        raise ValueError("invalid output rate")
    if data.get("restorations") != 0:
        raise ValueError("snapshot runs are correctness tests, not playback benchmarks")
    elapsed, ratios = 0.0, []
    for window in data["windows"]:
        clocks, seconds = window["master_clocks"], window["seconds"]
        if (not isinstance(clocks, int) or clocks < MASTER_HZ or
                not math.isfinite(seconds) or seconds <= 0):
            raise ValueError("invalid full playback window")
        if elapsed >= warmup:
            ratios.append(clocks / MASTER_HZ / seconds)
        elapsed += clocks / MASTER_HZ
    if len(ratios) < minimum_windows:
        raise ValueError("not enough post-warmup playback windows")
    ratios.sort()
    return {"windows": len(ratios), "median": statistics.median(ratios),
            "p05": ratios[math.floor((len(ratios)-1)*0.05)], "worst": ratios[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--minimum-ratio", type=float, default=1.5)
    parser.add_argument("--worst-ratio", type=float, default=1.2)
    parser.add_argument("--warmup-seconds", type=float, default=10.0)
    parser.add_argument("--minimum-windows", type=int, default=30)
    args = parser.parse_args()
    if (not all(math.isfinite(x) and x >= 0 for x in
                (args.minimum_ratio, args.worst_ratio, args.warmup_seconds)) or
            args.minimum_windows < 1 or args.minimum_ratio < 1 or args.worst_ratio < 1):
        parser.error("headroom thresholds must be finite and at least realtime")
    failed, coverage = False, set()
    for path in args.reports:
        try:
            data = json.loads(path.read_text())
            result = summarize(data, args.warmup_seconds, args.minimum_windows)
            coverage.add((data["model"], data["combined"]))
            passed = result["p05"] >= args.minimum_ratio and result["worst"] >= args.worst_ratio
            failed |= not passed
            print(f"{data['model']}/{'combined' if data['combined'] else 'native'} "
                  f"{data['output_hz']} Hz: median {result['median']:.2f}x, "
                  f"p05 {result['p05']:.2f}x, worst {result['worst']:.2f}x "
                  f"({result['windows']} windows): {'PASS' if passed else 'FAIL'}")
        except (ValueError, KeyError, TypeError, OSError) as error:
            failed = True
            print(f"{path}: FAIL: {error}")
    if coverage != {(model, mixed) for model in ("sgb1", "sgb2") for mixed in (False, True)}:
        print("FAIL: both models need native and combined playback reports")
        failed = True
    print("Host-only results exclude frontend rendering and audio-device playback.")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
