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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?", type=Path)
    parser.add_argument("references", nargs="*", type=Path)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--sort", choices=("edges", "rgb"), default="edges")
    parser.add_argument("--target-series", help="glob for local GBB PPM/PNG frames")
    parser.add_argument("--reference-series", help="glob for independent frames")
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    try:
        if args.target_series or args.reference_series:
            if not args.target_series or not args.reference_series or args.target or args.references:
                parser.error("series mode requires both globs and no positional paths")
            targets = [Path(path) for path in glob.glob(args.target_series)]
            references = [Path(path) for path in glob.glob(args.reference_series)]
            matches = exact_scenes(targets, references)
            print(json.dumps({
                "target_frames": len(targets),
                "reference_frames": len(references),
                "exact_scenes": matches,
            }, indent=2))
            return 0 if matches else 1
        if args.target is None or not args.references:
            parser.error("a target and at least one reference are required")
        results = rank_frames(args.target, args.references, args.sort)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, f"error: {error}\n")
    print(json.dumps(results[:args.top], indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
