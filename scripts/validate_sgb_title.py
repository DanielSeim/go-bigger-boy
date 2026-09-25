#!/usr/bin/env python3
"""Capture a locally supplied SGB title and check pinned, external evidence.

The manifest never contains ROM bytes. A run with no independent reference is
an inventory, not a visual validation, even if its command expectations pass.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import zlib

from compare_voxel_screenshots import _load_image
from report_sgb_commands import COMMANDS, count_commands


SHA256 = re.compile(r"[0-9a-f]{64}\Z")
COMMAND_ID = re.compile(r"0x[0-9A-F]{2}\Z")
MAX_ROM_BYTES = 16 * 1024 * 1024


def positive_integer(value: object, name: str) -> int:
    if type(value) is not int or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def load_manifest(path: Path) -> dict[str, object]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or type(manifest.get("schema")) is not int or manifest["schema"] != 1:
        raise ValueError("expected an SGB title manifest with schema 1")
    if not isinstance(manifest.get("title"), str) or not manifest["title"].strip():
        raise ValueError("title must be a non-empty string")
    if manifest.get("model") not in ("sgb", "sgb2"):
        raise ValueError("model must be sgb or sgb2")
    digest = manifest.get("rom_sha256")
    if not isinstance(digest, str) or not SHA256.fullmatch(digest):
        raise ValueError("rom_sha256 must be a lowercase SHA-256 digest")
    positive_integer(manifest.get("frames"), "frames")
    positive_integer(manifest.get("max_cycles"), "max_cycles")
    minimums = manifest.get("command_minimums")
    if not isinstance(minimums, dict) or not minimums:
        raise ValueError("command_minimums must contain at least one command")
    for command, minimum in minimums.items():
        if not isinstance(command, str) or not COMMAND_ID.fullmatch(command):
            raise ValueError(f"invalid command ID: {command}")
        positive_integer(minimum, f"minimum for {command}")
    input_data = manifest.get("input")
    if input_data is not None:
        if not isinstance(input_data, dict) or not isinstance(input_data.get("script"), str) or not input_data["script"].strip():
            raise ValueError("input script path is required")
        if not isinstance(input_data.get("script_sha256"), str) or not SHA256.fullmatch(input_data["script_sha256"]):
            raise ValueError("input script_sha256 must be a lowercase SHA-256 digest")
    reference = manifest.get("reference")
    if reference is not None:
        if not isinstance(reference, dict) or reference.get("source") not in (
                "hardware", "independent-emulator"):
            raise ValueError("reference source must be hardware or independent-emulator")
        if not isinstance(reference.get("description"), str) or not reference["description"].strip():
            raise ValueError("reference description must identify the capture")
        digest_only = "frame_sha256" in reference
        if digest_only:
            if any(key in reference for key in ("image", "image_sha256", "region",
                                                 "channel_tolerance", "max_mismatched_pixels")):
                raise ValueError("frame_sha256 reference cannot include image comparison fields")
            if not isinstance(reference["frame_sha256"], str) or not SHA256.fullmatch(reference["frame_sha256"]):
                raise ValueError("reference frame_sha256 must be a lowercase SHA-256 digest")
        else:
            if not isinstance(reference.get("image"), str) or not reference["image"].strip():
                raise ValueError("reference image path is required")
            if not isinstance(reference.get("image_sha256"), str) or not SHA256.fullmatch(reference["image_sha256"]):
                raise ValueError("reference image_sha256 must be a lowercase SHA-256 digest")
            if reference.get("region") not in ("full", "viewport"):
                raise ValueError("reference region must be full or viewport")
        tolerance = reference.get("channel_tolerance", 0)
        allowed = reference.get("max_mismatched_pixels", 0)
        if type(tolerance) is not int or not 0 <= tolerance <= 255:
            raise ValueError("channel_tolerance must be in 0..255")
        if type(allowed) is not int or allowed < 0:
            raise ValueError("max_mismatched_pixels must be nonnegative")
    return manifest


def file_digest(path: Path) -> str:
    if path.stat().st_size > MAX_ROM_BYTES:
        raise ValueError(f"{path}: input exceeds the 16 MiB validation limit")
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for block in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def compare_reference(actual_path: Path, reference_path: Path,
                      region: str, tolerance: int,
                      allowed: int) -> dict[str, object]:
    actual_width, actual_height, actual = _load_image(actual_path)
    if (actual_width, actual_height) != (256, 224):
        raise ValueError("SGB capture must be 256x224")
    width, height, expected = _load_image(reference_path)
    expected_size = (256, 224) if region == "full" else (160, 144)
    if (width, height) != expected_size:
        raise ValueError(f"{region} reference must be {expected_size[0]}x{expected_size[1]}")
    x_offset, y_offset = (0, 0) if region == "full" else (48, 40)
    mismatches = 0
    first = None
    for y in range(height):
        for x in range(width):
            actual_offset = ((y + y_offset) * actual_width + x + x_offset) * 3
            reference_offset = (y * width + x) * 3
            if any(abs(actual[actual_offset + channel] -
                       expected[reference_offset + channel]) > tolerance
                   for channel in range(3)):
                mismatches += 1
                if first is None:
                    first = [x, y]
    return {
        "passed": mismatches <= allowed,
        "region": region,
        "mismatched_pixels": mismatches,
        "max_mismatched_pixels": allowed,
        "first_mismatch": first,
    }


def validate(manifest_path: Path, rom: Path, runner: Path,
             output_dir: Path) -> dict[str, object]:
    manifest = load_manifest(manifest_path)
    actual_hash = file_digest(rom)
    if actual_hash != manifest["rom_sha256"]:
        raise ValueError("ROM SHA-256 does not match the title manifest")
    input_data = manifest.get("input")
    script_path = None
    if isinstance(input_data, dict):
        script_path = Path(input_data["script"])
        if not script_path.is_absolute():
            script_path = manifest_path.resolve().parent / script_path
        if file_digest(script_path) != input_data["script_sha256"]:
            raise ValueError("input script SHA-256 does not match the manifest")
    output_dir.mkdir(parents=True, exist_ok=True)
    frame_path = output_dir / "sgb-frame.ppm"
    trace_path = output_dir / "sgb.trace"
    command = [str(runner.resolve()), str(rom.resolve()),
               "--model", str(manifest["model"]),
               "--frames", str(manifest["frames"]),
               "--max-cycles", str(manifest["max_cycles"]),
               "--sgb-frame", "--frame-output", str(frame_path),
               "--sgb-trace", str(trace_path)]
    if script_path is not None:
        command.extend(["--input-script", str(script_path)])
    completed = subprocess.run(command, capture_output=True, text=True,
                               timeout=180, check=False)
    if completed.returncode != 0:
        raise RuntimeError("SGB capture failed: " +
                           (completed.stderr or completed.stdout)[-2000:])
    width, height, _ = _load_image(frame_path)
    if (width, height) != (256, 224):
        raise ValueError("runner did not capture a 256x224 SGB frame")
    counts = count_commands(trace_path)
    minimums = manifest["command_minimums"]
    assert isinstance(minimums, dict)
    missing = {command: minimum for command, minimum in minimums.items()
               if counts[int(command, 16)] < minimum}
    reference = manifest.get("reference")
    comparison = None
    if isinstance(reference, dict):
        if "frame_sha256" in reference:
            comparison = {
                "passed": file_digest(frame_path) == reference["frame_sha256"],
                "region": "full", "exact_frame_digest": True,
            }
        else:
            image = Path(reference["image"])
            if not image.is_absolute():
                image = manifest_path.resolve().parent / image
            if file_digest(image) != reference["image_sha256"]:
                raise ValueError("reference image SHA-256 does not match the manifest")
            comparison = compare_reference(
                frame_path, image, reference["region"],
                reference.get("channel_tolerance", 0),
                reference.get("max_mismatched_pixels", 0))
    passed = not missing and (comparison is None or comparison["passed"])
    report = {
        "schema": "gbb.sgb.title-validation.v1",
        "title": manifest["title"],
        "model": manifest["model"],
        "rom_sha256": actual_hash,
        "input_script_sha256": None if input_data is None else input_data["script_sha256"],
        "frames": manifest["frames"],
        "status": ("failed" if not passed else
                   "validated" if comparison is not None else "inventory_only"),
        "commands": {
            f"0x{command_id:02X}": {
                "name": COMMANDS.get(command_id, ("UNKNOWN", ""))[0],
                "count": count,
                "support": COMMANDS.get(command_id, ("UNKNOWN", "unimplemented"))[1],
            } for command_id, count in sorted(counts.items())
        },
        "missing_command_minimums": missing,
        "reference": None if comparison is None else {
            "source": reference["source"],
            "description": reference["description"],
            "comparison": comparison,
            **({"frame_sha256": reference["frame_sha256"]}
               if "frame_sha256" in reference else
               {"image_sha256": reference["image_sha256"]}),
        },
        "frame": str(frame_path),
        "frame_sha256": file_digest(frame_path),
        "trace": str(trace_path),
    }
    (output_dir / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        report = validate(args.manifest, args.rom, args.runner, args.output_dir)
    except (OSError, ValueError, RuntimeError, zlib.error,
            subprocess.TimeoutExpired) as error:
        print(f"SGB title validation error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if report["status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
