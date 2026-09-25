#!/usr/bin/env python3
"""Rank independent SGB frames by color-independent edges before pixel checks.

This is scene alignment only, not a correctness oracle. A low edge score can
still conceal wrong colors or missing SGB host effects. The input frames stay
local and are never copied into this repository.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import glob
import hashlib
import json
from pathlib import Path
import sys

from compare_voxel_screenshots import _load_image


WIDTH = 256
HEIGHT = 224
VIEWPORT = (48, 40, 160, 144)


def read_sgb_image(path: Path) -> bytes:
    width, height, pixels = _load_image(path)
    if (width, height) != (WIDTH, HEIGHT):
        raise ValueError(f"{path}: expected a 256x224 SGB frame")
    return pixels


def edges(pixels: bytes, region: tuple[int, int, int, int]) -> bytes:
    x0, y0, width, height = region
    result = bytearray()
    for y in range(y0, y0 + height - 1):
        for x in range(x0, x0 + width - 1):
            position = (y * WIDTH + x) * 3
            rgb = pixels[position:position + 3]
            result.append(
                (rgb != pixels[position + 3:position + 6]) |
                ((rgb != pixels[position + WIDTH * 3:
                                position + WIDTH * 3 + 3]) << 1))
    return bytes(result)


def rank_frames(target: Path, references: list[Path],
                priority: str = "edges") -> list[dict[str, object]]:
    if not references:
        raise ValueError("at least one reference frame is required")
    target_pixels = read_sgb_image(target)
    target_view_edges = edges(target_pixels, VIEWPORT)
    target_full_edges = edges(target_pixels, (0, 0, WIDTH, HEIGHT))
    results: list[dict[str, object]] = []
    for reference in references:
        pixels = read_sgb_image(reference)
        view_edges = edges(pixels, VIEWPORT)
        full_edges = edges(pixels, (0, 0, WIDTH, HEIGHT))
        view_mismatches = sum(left != right for left, right in
                              zip(target_view_edges, view_edges))
        full_mismatches = sum(left != right for left, right in
                              zip(target_full_edges, full_edges))
        rgb_mismatches = sum(target_pixels[index:index + 3] !=
                             pixels[index:index + 3]
                             for index in range(0, len(pixels), 3))
        results.append({
            "reference": str(reference),
            "viewport_edge_mismatches": view_mismatches,
            "full_edge_mismatches": full_mismatches,
            "rgb_mismatches": rgb_mismatches,
        })
    if priority == "rgb":
        return sorted(results, key=lambda result: (
            result["rgb_mismatches"],
            result["viewport_edge_mismatches"],
            result["full_edge_mismatches"]))
    if priority != "edges":
        raise ValueError("priority must be edges or rgb")
    return sorted(results, key=lambda result: (
        result["viewport_edge_mismatches"],
        result["full_edge_mismatches"], result["rgb_mismatches"]))


def exact_scenes(targets: list[Path],
                 references: list[Path]) -> list[dict[str, object]]:
    if not targets or not references:
        raise ValueError("both frame series must contain at least one image")
    by_hash: dict[str, dict[str, list[str]]] = defaultdict(
        lambda: {"targets": [], "references": []})
    for kind, paths in (("targets", targets), ("references", references)):
        for path in paths:
            digest = hashlib.sha256(read_sgb_image(path)).hexdigest()
            by_hash[digest][kind].append(str(path))
    matches = []
    for digest, paths in by_hash.items():
        if paths["targets"] and paths["references"]:
            matches.append({
                "pixel_sha256": digest,
                "target": min(paths["targets"]),
                "reference": min(paths["references"]),
                "target_count": len(paths["targets"]),
                "reference_count": len(paths["references"]),
            })
    return sorted(matches, key=lambda match: match["target"])


def _numbered_frames(paths: list[Path]) -> dict[int, Path]:
    numbered: dict[int, Path] = {}
    for path in paths:
        suffix = path.stem.rsplit("-", 1)[-1]
        if not suffix.isdigit():
            raise ValueError(f"{path}: expected a trailing frame number")
        frame = int(suffix)
        if frame in numbered:
            raise ValueError(f"duplicate frame number {frame}")
        numbered[frame] = path
    if not numbered:
        raise ValueError("frame series is empty")
    return numbered


def compare_sequences(targets: list[Path], references: list[Path],
                      offset: int, window: int) -> dict[str, object]:
    """Compare every target frame to an independent frame near its offset.

    Bounded scene alignment absorbs a few capture-boundary frames of drift;
    unmatched frames remain failures rather than being silently skipped.
    """
    if not 0 <= window <= 60:
        raise ValueError("sequence window must be in 0..60")
    target_frames = _numbered_frames(targets)
    reference_frames = _numbered_frames(references)
    target_hashes = {number: hashlib.sha256(read_sgb_image(path)).hexdigest()
                     for number, path in target_frames.items()}
    reference_hashes = {number: hashlib.sha256(read_sgb_image(path)).hexdigest()
                        for number, path in reference_frames.items()}
    matched = 0
    mismatches: list[dict[str, object]] = []
    largest_shift = 0
    for frame in sorted(target_frames):
        expected = frame + offset
        candidates = [number for number in reference_frames
                      if expected - window <= number <= expected + window]
        matches = [number for number in candidates
                   if reference_hashes[number] == target_hashes[frame]]
        if matches:
            chosen = min(matches, key=lambda number: (abs(number - expected), number))
            largest_shift = max(largest_shift, abs(chosen - expected))
            matched += 1
        else:
            mismatch: dict[str, object] = {
                "frame": frame,
                "expected_reference_frame": expected,
                "target_pixel_sha256": target_hashes[frame],
                "reference_frames_checked": len(candidates),
            }
            if candidates:
                actual = read_sgb_image(target_frames[frame])
                best = None
                for candidate in candidates:
                    reference_pixels = read_sgb_image(reference_frames[candidate])
                    differing = [index for index in range(0, len(actual), 3)
                                 if actual[index:index + 3] !=
                                 reference_pixels[index:index + 3]]
                    rank = (len(differing), abs(candidate - expected), candidate)
                    if best is None or rank < best[0]:
                        best = (rank, differing)
                assert best is not None
                mismatch["nearest_reference_frame"] = best[0][2]
                mismatch["mismatched_pixels"] = best[0][0]
                mismatch["first_pixel"] = (
                    [best[1][0] // 3 % WIDTH, best[1][0] // 3 // WIDTH]
                    if best[1] else None)
            mismatches.append(mismatch)
    return {
        "passed": not mismatches,
        "target_frames": len(target_frames),
        "reference_frames": len(reference_frames),
        "unique_target_scenes": len(set(target_hashes.values())),
        "matched_frames": matched,
        "unmatched_frames": len(mismatches),
        "first_mismatch": mismatches[0] if mismatches else None,
        "mismatches": mismatches[:20],
        "offset": offset,
        "window": window,
        "largest_matched_shift": largest_shift,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?", type=Path)
    parser.add_argument("references", nargs="*", type=Path)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--sort", choices=("edges", "rgb"), default="edges")
    parser.add_argument("--target-series", help="glob for local GBB PPM/PNG frames")
    parser.add_argument("--reference-series", help="glob for independent frames")
    parser.add_argument("--sequence-offset", type=int,
                        help="compare every numbered target frame near frame + OFFSET")
    parser.add_argument("--window", type=int, default=0,
                        help="maximum independent-frame shift in sequence mode")
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    try:
        if args.target_series or args.reference_series:
            if not args.target_series or not args.reference_series or args.target or args.references:
                parser.error("series mode requires both globs and no positional paths")
            targets = [Path(path) for path in glob.glob(args.target_series)]
            references = [Path(path) for path in glob.glob(args.reference_series)]
            if args.sequence_offset is not None:
                result = compare_sequences(targets, references,
                                           args.sequence_offset, args.window)
                print(json.dumps(result, indent=2))
                return 0 if result["passed"] else 1
            if args.window:
                parser.error("--window requires --sequence-offset")
            matches = exact_scenes(targets, references)
            print(json.dumps({
                "target_frames": len(targets),
                "reference_frames": len(references),
                "exact_scenes": matches,
            }, indent=2))
            return 0 if matches else 1
        if args.target is None or not args.references:
            parser.error("a target and at least one reference are required")
        if args.sequence_offset is not None or args.window:
            parser.error("sequence options require both series globs")
        results = rank_frames(args.target, args.references, args.sort)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, f"error: {error}\n")
    print(json.dumps(results[:args.top], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
