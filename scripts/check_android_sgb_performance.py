#!/usr/bin/env python3
"""Gate a steady-state Android SGB run using GBB frame_timing log lines."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


FIELD = re.compile(r"([a-z_]+)=([0-9.]+)")
MODE = re.compile(r"\bmode=([a-z_]+)")


def samples(text: str, mode: str | None = None) -> list[dict[str, float]]:
    result = []
    for line in text.splitlines():
        if "GBB frame_timing" not in line:
            continue
        detected_mode = MODE.search(line)
        if mode is not None and (detected_mode is None or
                                 detected_mode.group(1) != mode):
            continue
        fields = {name: float(value) for name, value in FIELD.findall(line)}
        if fields.get("sgb_frames", 0) > 0 and "fps" in fields:
            result.append(fields)
    return result


def evaluate(text: str, minimum_fps: float, warmup_windows: int,
             minimum_windows: int, mode: str | None = None) -> tuple[bool, str]:
    measured = samples(text, mode)[warmup_windows:]
    if len(measured) < minimum_windows:
        return False, (f"only {len(measured)} steady-state SGB timing windows; "
                       f"need {minimum_windows}")
    fps = [sample["fps"] for sample in measured]
    worst = min(fps)
    average = sum(fps) / len(fps)
    slowest_run = 0
    current_run = 0
    for value in fps:
        current_run = current_run + 1 if value < minimum_fps else 0
        slowest_run = max(slowest_run, current_run)
    fields = ("core_step_avg_us", "sgb_compose_avg_us",
              "sgb_transform_avg_us", "sgb_upload_avg_us",
              "sgb_present_avg_us")
    stage_summary = " ".join(
        f"{name}={sum(sample.get(name, 0) for sample in measured) / len(measured):.0f}"
        for name in fields)
    passed = average >= minimum_fps and slowest_run < 3
    status = "PASS" if passed else "FAIL"
    return passed, (
        f"{status}: SGB mode={mode or 'all'} windows={len(measured)} "
        f"worst_fps={worst:.2f} average_fps={average:.2f} "
        f"consecutive_slow_windows={slowest_run} floor={minimum_fps:.2f} "
        f"{stage_summary}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path,
                        help="ADB logcat text; reads stdin when omitted")
    parser.add_argument("--mode", choices=("nearest", "voxel", "voxel_shape",
                                           "voxel_popup"),
                        help="evaluate only one video mode")
    parser.add_argument("--min-fps", type=float, default=59.0,
                        help="steady-state floor (GB nominal cadence is 59.73 Hz)")
    parser.add_argument("--warmup-windows", type=int, default=1)
    parser.add_argument("--min-windows", type=int, default=3)
    args = parser.parse_args()
    if args.min_fps <= 0 or args.warmup_windows < 0 or args.min_windows <= 0:
        parser.error("thresholds and window counts must be positive")
    try:
        contents = args.input.read_text(encoding="utf-8", errors="replace") if args.input else sys.stdin.read()
    except OSError as error:
        parser.error(str(error))
    passed, report = evaluate(contents, args.min_fps,
                              args.warmup_windows, args.min_windows, args.mode)
    print(report)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
