#!/usr/bin/env python3
"""Compare SDL render benchmark results with conservative platform baselines."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def evaluate_report(
    report: dict[str, Any], baseline: dict[str, Any], platform: str
) -> dict[str, Any]:
    platforms = baseline.get("platforms", {})
    platform_baseline = platforms.get(platform)
    if not isinstance(platform_baseline, dict):
        raise ValueError(f"no render-performance baseline for platform {platform}")
    ratio_floor = float(baseline.get("minimum_ratio", 0.65))
    if not 0.0 < ratio_floor <= 1.0:
        raise ValueError("minimum_ratio must be between zero and one")

    modes = report.get("modes")
    if not isinstance(modes, list) or not modes:
        raise ValueError("benchmark report contains no modes")

    results = []
    for item in modes:
        if not isinstance(item, dict):
            raise ValueError("benchmark mode entry is not an object")
        mode = item.get("mode")
        fps = item.get("fps")
        if not isinstance(mode, str) or not isinstance(fps, (int, float)):
            raise ValueError("benchmark mode entry has invalid mode or fps")
        if mode not in platform_baseline:
            raise ValueError(f"baseline is missing mode {mode}")
        baseline_fps = float(platform_baseline[mode])
        minimum_fps = baseline_fps * ratio_floor
        passed = float(fps) >= minimum_fps
        results.append(
            {
                "mode": mode,
                "fps": float(fps),
                "baseline_fps": baseline_fps,
                "minimum_fps": minimum_fps,
                "ratio": float(fps) / baseline_fps if baseline_fps else None,
                "status": "pass" if passed else "fail",
                "elapsed_ms": item.get("elapsed_ms"),
                "mesh_vertices": item.get("mesh_vertices", 0),
                "mesh_indices": item.get("mesh_indices", 0),
            }
        )
    return {
        "schema_version": 1,
        "platform": platform,
        "minimum_ratio": ratio_floor,
        "status": "pass" if all(x["status"] == "pass" for x in results) else "fail",
        "results": results,
    }


def markdown_report(evaluation: dict[str, Any]) -> str:
    lines = [
        "# SDL rendering performance report",
        "",
        f"Platform: `{evaluation['platform']}`  ",
        f"Minimum accepted baseline ratio: `{evaluation['minimum_ratio']:.2f}`",
        "",
        "| Mode | FPS | Baseline | Minimum | Ratio | Status |",
        "| --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for item in evaluation["results"]:
        lines.append(
            f"| `{item['mode']}` | {item['fps']:.2f} | "
            f"{item['baseline_fps']:.2f} | {item['minimum_fps']:.2f} | "
            f"{item['ratio']:.2f} | **{item['status']}** |"
        )
    lines.extend(
        [
            "",
            "The benchmark uses a warmed-up SDL software renderer. A failed "
            "mode indicates a sustained performance regression against the "
            "conservative platform baseline.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--markdown-out", type=Path, required=True)
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="write a skipped report when the SDL benchmark could not run",
    )
    args = parser.parse_args()

    if not args.input.exists() and args.allow_missing:
        evaluation = {
            "schema_version": 1,
            "platform": args.platform,
            "minimum_ratio": None,
            "status": "skipped",
            "results": [],
            "reason": "SDL render benchmark did not produce an input report",
        }
        args.markdown_out.write_text(
            "# SDL rendering performance report\n\n"
            "Status: **skipped** (benchmark produced no input report).\n",
            encoding="utf-8",
        )
        args.json_out.write_text(
            json.dumps(evaluation, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print("SDL rendering performance benchmark skipped")
        return 0

    try:
        report = json.loads(args.input.read_text(encoding="utf-8"))
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        evaluation = evaluate_report(report, baseline, args.platform)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"render performance report failed: {error}")
        return 2

    args.markdown_out.write_text(markdown_report(evaluation), encoding="utf-8")
    args.json_out.write_text(
        json.dumps(evaluation, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(markdown_report(evaluation))
    return 0 if evaluation["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
