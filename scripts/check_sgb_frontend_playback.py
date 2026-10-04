#!/usr/bin/env python3
"""Check completed firmware frontend traces, not audible fidelity or host-only speed."""
import argparse
import json
import math
from pathlib import Path
import statistics


def fields(line):
    return dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)


def evaluate(text, warmup_seconds=10, minimum_seconds=60, allow_dummy=False):
    headers, ends, samples = [], [], []
    if not any(line.startswith("build=release") for line in text.splitlines()):
        raise ValueError("qualification requires a Release build trace")
    for line in text.splitlines():
        if line.startswith("frame_timing ") and fields(line).get("core_steps") == "0":
            raise ValueError("paused/lifecycle traces cannot qualify steady-state playback")
        if line.startswith("firmware_qualification "):
            headers.append(fields(line))
        elif line.startswith("firmware_qualification_complete "):
            ends.append(fields(line))
        elif line.startswith("firmware_frame "):
            samples.append(fields(line))
    if len(headers) != 1 or len(ends) != 1:
        raise ValueError("missing, duplicate or incomplete qualification run")
    header = headers[0]
    count = int(header["frames"])
    if (header["version"] != "1" or header["model"] not in ("sgb", "sgb2") or
            count != len(samples) or count != int(ends[0]["frames"]) or count < 2):
        raise ValueError("invalid model/version or truncated frame samples")
    previous, elapsed, intervals, work, measured = 0, 0, [], [], []
    warmup_us = warmup_seconds * 1_000_000
    for index, sample in enumerate(samples, 1):
        elapsed, busy = int(sample["elapsed_us"]), int(sample["work_us"])
        interval = elapsed - previous
        if int(sample["index"]) != index or interval <= 0 or busy < 0 or busy > interval:
            raise ValueError("non-monotonic, missing or invalid frame sample")
        if previous >= warmup_us:
            intervals.append(interval)
            work.append(busy)
            measured.append(sample)
        for stage in ("events_us", "emulation_us", "present_us"):
            if stage in sample and not 0 <= int(sample[stage]) <= busy:
                raise ValueError("invalid per-frame stage timing")
        previous = elapsed
    seconds = sum(intervals) / 1_000_000
    if seconds < minimum_seconds or not intervals:
        raise ValueError("insufficient post-warmup playback duration")
    empty = int(header["audio_empty_queue_events"])
    resets = int(header["audio_latency_resets"])
    if (empty < 0 or resets < 0 or header["audio_available"] not in ("0", "1") or
            header["audio_enabled"] not in ("0", "1")):
        raise ValueError("invalid audio counters")
    driver = header["audio_driver"]
    real_audio = header["audio_available"] == "1" and driver not in ("dummy", "disk", "none", "")
    ordered = sorted(intervals)
    p99 = ordered[math.ceil(len(ordered) * .99) - 1] / 1000
    worst = max(intervals) / 1000
    worst_sample = measured[intervals.index(max(intervals))]
    fps = len(intervals) / seconds
    # Pace at the unchanged 60.098 Hz SNES clock. Small scheduler jitter is
    # tolerated; frame tails and resets prevent average FPS hiding stalls.
    passed = 59.5 <= fps <= 60.5 and p99 <= 25 and worst <= 100 and resets == 0
    passed &= header["audio_enabled"] == "1" and (real_audio or (allow_dummy and header["audio_available"] == "1"))
    return {"model": header["model"], "seconds": seconds, "frames": len(intervals),
            "fps": fps, "frame_p99_ms": p99, "frame_worst_ms": worst,
            "work_median_ms": statistics.median(work) / 1000,
            "worst_frame": {"index": int(worst_sample["index"]),
                            **{key: int(worst_sample[key]) for key in
                               ("work_us", "events_us", "emulation_us", "present_us") if key in worst_sample}},
            "audio_driver": driver, "real_audio_backend": real_audio,
            "empty_input_queue_observations": empty, "latency_resets": resets,
            "passed": bool(passed)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", type=Path, nargs="+")
    parser.add_argument("--warmup-seconds", type=float, default=10)
    parser.add_argument("--minimum-seconds", type=float, default=60)
    parser.add_argument("--allow-dummy", action="store_true", help="plumbing tests only, not device qualification")
    args = parser.parse_args()
    if (not math.isfinite(args.warmup_seconds) or args.warmup_seconds < 0 or
            not math.isfinite(args.minimum_seconds) or args.minimum_seconds <= 0):
        parser.error("durations must be finite; minimum duration must be positive")
    failed, models = False, set()
    for path in args.traces:
        try:
            result = evaluate(path.read_text(), args.warmup_seconds, args.minimum_seconds, args.allow_dummy)
            models.add(result["model"])
            failed |= not result["passed"]
            print(json.dumps({"trace": str(path), **result}, sort_keys=True))
        except (OSError, ValueError, KeyError, TypeError) as error:
            failed = True
            print(f"{path}: FAIL: {error}")
    if models != {"sgb", "sgb2"}:
        print("FAIL: completed SGB1 and SGB2 traces are both required")
        failed = True
    print("Queue observations are not hardware underruns. Listening and lifecycle checks remain separate.")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
