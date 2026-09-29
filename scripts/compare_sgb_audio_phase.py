#!/usr/bin/env python3
"""Align SGB title video transitions without assuming matching RGB palettes.

This is a visual phase diagnostic. Matching transitions do not prove that a
mixed-audio reference contains the same SOUND command at that frame.
"""

import argparse
import glob
import json
from pathlib import Path
import sys

from align_sgb_frames import read_sgb_image, VIEWPORT, WIDTH


def viewport(path: Path) -> bytes:
    pixels = read_sgb_image(path)
    x, y, width, height = VIEWPORT
    return b"".join(pixels[(row * WIDTH + x) * 3:
                           (row * WIDTH + x + width) * 3]
                    for row in range(y, y + height))


def series_transitions(pattern: str, min_changed_pixels: int
                       ) -> tuple[tuple[int, int], dict[int, int], int]:
    paths = [Path(name) for name in glob.glob(pattern)]
    if len(paths) < 3:
        raise ValueError("a phase series needs at least three frames")
    frames = {}
    for path in paths:
        suffix = path.stem.rsplit("-", 1)[-1]
        if not suffix.isdecimal():
            raise ValueError(f"{path}: expected a trailing frame number")
        number = int(suffix)
        if number in frames:
            raise ValueError(f"duplicate frame {number} in {pattern}")
        frames[number] = path
    numbers = sorted(frames)
    if numbers[-1] - numbers[0] + 1 != len(numbers):
        raise ValueError("phase series must have no missing frames")
    previous = viewport(frames[numbers[0]])
    all_changes = {}
    for number in numbers[1:]:
        current = viewport(frames[number])
        changed = sum(previous[index:index + 3] != current[index:index + 3]
                      for index in range(0, len(current), 3))
        all_changes[number] = changed
        previous = current
    threshold = min_changed_pixels or max(16, max(all_changes.values()) // 2)
    transitions = {frame: count for frame, count in all_changes.items()
                   if count >= threshold}
    return (numbers[0], numbers[-1]), transitions, threshold


def rank_offsets(gbb_range: tuple[int, int], gbb: dict[int, int],
                 reference_range: tuple[int, int], reference: dict[int, int],
                 expected_offset: int, window: int) -> list[dict[str, object]]:
    if window < 0 or window > 120:
        raise ValueError("offset window must be in 0..120")
    if gbb_range[0] >= gbb_range[1] or reference_range[0] >= reference_range[1]:
        raise ValueError("invalid phase frame range")
    if len(gbb) < 2 or len(reference) < 2:
        raise ValueError("at least two significant transitions are needed per series")
    results = []
    for offset in range(expected_offset - window, expected_offset + window + 1):
        first = max(gbb_range[0] + offset, reference_range[0])
        last = min(gbb_range[1] + offset, reference_range[1])
        if last - first + 1 < (gbb_range[1] - gbb_range[0] + 1) * 3 // 4:
            continue
        target = {frame + offset for frame in gbb if first <= frame + offset <= last}
        observed = {frame for frame in reference if first <= frame <= last}
        matches = sorted(target & observed)
        missed = sorted(target - observed)
        extras = sorted(observed - target)
        if len(target) < 2 or len(observed) < 2:
            continue
        results.append({"offset": offset, "matched_reference_frames": matches,
                        "missed_gbb_transitions": missed,
                        "extra_reference_transitions": extras,
                        "matched": len(matches), "disagreements": len(missed) + len(extras),
                        "overlap_frames": last - first + 1})
    if not results:
        raise ValueError("no offset has sufficient series overlap")
    return sorted(results, key=lambda item: (-item["matched"], item["disagreements"],
                                             -item["overlap_frames"],
                                             abs(item["offset"] - expected_offset)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb-series", required=True, help="glob for numbered GBB frames")
    parser.add_argument("--reference-series", required=True,
                        help="glob for numbered reference frames")
    parser.add_argument("--expected-offset", required=True, type=int,
                        help="reference frame minus GBB frame near the event")
    parser.add_argument("--window", type=int, default=10)
    parser.add_argument("--min-changed-pixels", type=int, default=0,
                        help="significant viewport transition threshold; 0 uses half the largest change")
    args = parser.parse_args()
    try:
        if not 0 <= args.min_changed_pixels <= VIEWPORT[2] * VIEWPORT[3]:
            raise ValueError("invalid changed-pixel threshold")
        gbb_range, gbb, gbb_threshold = series_transitions(
            args.gbb_series, args.min_changed_pixels)
        reference_range, reference, reference_threshold = series_transitions(
            args.reference_series, args.min_changed_pixels)
        ranked = rank_offsets(gbb_range, gbb, reference_range, reference,
                              args.expected_offset, args.window)
        best = ranked[0]
        # Multiple equally good transition alignments mean phase is unresolved.
        tied = [item for item in ranked if (item["matched"], item["disagreements"]) ==
                (best["matched"], best["disagreements"])]
        resolved = (len(tied) == 1 and best["matched"] >= 2 and
                    best["disagreements"] == 0)
        print(json.dumps({"resolved": resolved, "gbb_range": gbb_range,
                          "gbb_transitions": gbb, "gbb_threshold": gbb_threshold,
                          "reference_range": reference_range,
                          "reference_transitions": reference,
                          "reference_threshold": reference_threshold,
                          "best": best, "equally_scored_offsets":
                          [item["offset"] for item in tied]}, indent=2))
        return 0 if resolved else 1
    except (OSError, ValueError, RuntimeError) as error:
        print(f"phase comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
