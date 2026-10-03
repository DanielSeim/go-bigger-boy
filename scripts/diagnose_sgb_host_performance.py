#!/usr/bin/env python3
"""Explain playback CPU/scheduling measurements without adjusting headroom gates."""
import argparse
import json
import math
from pathlib import Path

from check_sgb_host_performance import summarize


def diagnostics(data):
    result = summarize(data)
    windows = data["windows"]
    wall = sum(window["seconds"] for window in windows)
    cpu = [window.get("cpu_seconds") for window in windows]
    if all(isinstance(value, (int, float)) and not isinstance(value, bool)
           and math.isfinite(value) and value >= 0 for value in cpu):
        result["cpu_wall_ratio"] = sum(cpu) / wall
    for field in ("voluntary_switches", "involuntary_switches"):
        values = [window.get(field) for window in windows]
        if all(type(value) is int and value >= 0 for value in values):
            result[field] = sum(values)
    endpoints = [(window.get("processor_before"), window.get("processor_after"))
                 for window in windows]
    if all(type(a) is int and type(b) is int and a >= 0 and b >= 0
           for a, b in endpoints):
        # These are sampled endpoints, not a complete migration count.
        result["changed_processor_endpoints"] = sum(a != b for a, b in endpoints)
    before = data.get("calibration_before_seconds")
    after = data.get("calibration_after_seconds")
    if all(isinstance(value, (int, float)) and not isinstance(value, bool)
           and math.isfinite(value) and value > 0 for value in (before, after)):
        result["calibration_after_before_ratio"] = after / before
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path)
    args = parser.parse_args()
    failed = False
    for path in args.reports:
        try:
            result = diagnostics(json.loads(path.read_text()))
            print(f"{path}: {json.dumps(result, sort_keys=True)}")
        except (ValueError, KeyError, TypeError, OSError) as error:
            print(f"{path}: invalid report: {error}")
            failed = True
    print("Diagnostics do not normalize wall time or override the playback gate. "
          "Calibration is comparable only with the same binary; processor endpoints "
          "do not enumerate all migrations.")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
