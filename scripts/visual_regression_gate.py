#!/usr/bin/env python3
"""Run a deterministic image capture and compare it with a backend baseline."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


MODES = ("nearest", "voxel", "voxel_shape", "voxel_popup")
COMPARATOR = Path(__file__).with_name("compare_voxel_screenshots.py")


def _load_comparator() -> Any:
    spec = importlib.util.spec_from_file_location("compare_voxel_screenshots", COMPARATOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load screenshot comparator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _image_for(directory: Path, mode: str) -> Path | None:
    for suffix in (".ppm", ".png"):
        candidate = directory / f"{mode}{suffix}"
        if candidate.is_file():
            return candidate
    return None


def compare_set(
    reference_dir: Path,
    actual_dir: Path,
    backend: str,
    modes: list[str],
    channel_tolerance: int,
    max_mismatch_fraction: float,
) -> dict[str, object]:
    comparator = _load_comparator()
    entries: list[dict[str, object]] = []
    for mode in modes:
        reference = _image_for(reference_dir, mode)
        actual = _image_for(actual_dir, mode)
        if reference is None or actual is None:
            entries.append({
                "mode": mode,
                "passed": False,
                "reason": "missing_capture",
                "reference": str(reference) if reference else None,
                "actual": str(actual) if actual else None,
            })
            continue
        comparison = comparator.compare_images(reference, actual, channel_tolerance)
        mismatch_fraction = float(comparison.get("mismatch_fraction", 1.0))
        passed = bool(comparison.get("passed")) and (
            mismatch_fraction <= max_mismatch_fraction
        )
        entries.append({
            **comparison,
            "mode": mode,
            "passed": passed,
            "max_mismatch_fraction": max_mismatch_fraction,
            "within_mismatch_budget": mismatch_fraction <= max_mismatch_fraction,
        })
    return {
        "schema": "gbb.visual-regression.v1",
        "backend": backend,
        "channel_tolerance": channel_tolerance,
        "max_mismatch_fraction": max_mismatch_fraction,
        "status": "pass" if all(item["passed"] for item in entries) else "fail",
        "modes": entries,
    }


def _write_report(path: Path | None, report: dict[str, object]) -> None:
    if path is not None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True)
    parser.add_argument("--reference-dir", required=True, type=Path)
    parser.add_argument("--actual-dir", required=True, type=Path)
    parser.add_argument("--mode", action="append", choices=MODES, dest="modes")
    parser.add_argument("--channel-tolerance", type=int, default=0)
    parser.add_argument("--max-mismatch-fraction", type=float, default=0.0)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--capture-command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    modes = args.modes or list(MODES)
    if not 0 <= args.channel_tolerance <= 255:
        parser.error("--channel-tolerance must be between 0 and 255")
    if not 0.0 <= args.max_mismatch_fraction <= 1.0:
        parser.error("--max-mismatch-fraction must be between zero and one")

    if args.capture_command:
        environment = os.environ.copy()
        environment["GBB_RENDER_PERF_CAPTURE_DIR"] = str(args.actual_dir)
        result = subprocess.run(args.capture_command, env=environment, check=False)
        if result.returncode == 77:
            return 77
        if result.returncode != 0:
            print(f"visual capture command failed with exit code {result.returncode}",
                  file=sys.stderr)
            return result.returncode or 2

    report = compare_set(
        args.reference_dir,
        args.actual_dir,
        args.backend,
        modes,
        args.channel_tolerance,
        args.max_mismatch_fraction,
    )
    _write_report(args.report, report)
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
