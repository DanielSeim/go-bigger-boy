#!/usr/bin/env python3
"""Compare a captured voxel screenshot with a checked-in reference image.

The command deliberately compares the complete rendered image rather than
only a crop. This catches viewport drift, controls moving into the viewport,
and changes to the pop-up geometry in one deterministic report.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _load_image(path: Path) -> tuple[int, int, bytes]:
    # Reuse the repository's dependency-free PNG/PPM readers so this check is
    # also available on minimal CI and Windows build machines.
    from importlib.util import module_from_spec, spec_from_file_location

    spec = spec_from_file_location("compare_frame", Path(__file__).parents[1] /
                                   "tests" / "compare_frame.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load image readers")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    if path.suffix.lower() == ".ppm":
        return module.read_ppm(path)
    return module.read_png(path)


def compare_images(reference: Path, actual: Path,
                   channel_tolerance: int = 0) -> dict[str, object]:
    reference_width, reference_height, expected = _load_image(reference)
    actual_width, actual_height, received = _load_image(actual)
    report: dict[str, object] = {
        "schema": "gbb.voxel.screenshot-comparison.v1",
        "reference": str(reference),
        "actual": str(actual),
        "width": actual_width,
        "height": actual_height,
        "channel_tolerance": channel_tolerance,
    }
    if (reference_width, reference_height) != (actual_width, actual_height):
        report.update({
            "passed": False,
            "reason": "dimensions",
            "reference_dimensions": [reference_width, reference_height],
            "actual_dimensions": [actual_width, actual_height],
        })
        return report
    mismatches = 0
    first_mismatch = None
    for pixel in range(actual_width * actual_height):
        offset = pixel * 3
        if any(abs(received[offset + channel] - expected[offset + channel]) >
               channel_tolerance for channel in range(3)):
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = [pixel % actual_width, pixel // actual_width]
    total = actual_width * actual_height
    report.update({
        "passed": mismatches == 0,
        "mismatched_pixels": mismatches,
        "pixel_count": total,
        "mismatch_fraction": mismatches / total if total else 0.0,
        "first_mismatch": first_mismatch,
    })
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--channel-tolerance", type=int, default=0)
    args = parser.parse_args()
    if args.channel_tolerance < 0 or args.channel_tolerance > 255:
        parser.error("--channel-tolerance must be between 0 and 255")
    try:
        report = compare_images(args.reference, args.actual,
                                args.channel_tolerance)
    except (OSError, ValueError, RuntimeError) as error:
        print(f"screenshot comparison failed: {error}", file=sys.stderr)
        return 2
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0 if report.get("passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
