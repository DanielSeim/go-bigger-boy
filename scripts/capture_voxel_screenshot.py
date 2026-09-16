#!/usr/bin/env python3
"""Capture a full-screen Android or desktop render for voxel comparison."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def capture_android(output: Path, adb: str, device: str | None) -> None:
    command = [adb]
    if device:
        command.extend(("-s", device))
    command.extend(("exec-out", "screencap", "-p"))
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.decode(errors="replace").strip())
    if not result.stdout.startswith(b"\x89PNG\r\n\x1a\n"):
        raise RuntimeError("adb returned no PNG screenshot")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result.stdout)


def capture_desktop(output: Path) -> None:
    # Keep this dependency-free at the Python level while supporting the
    # screenshot tools normally present on Linux, Windows and macOS hosts.
    candidates = [
        (["gnome-screenshot", "-f", str(output)], "gnome-screenshot"),
        (["scrot", str(output)], "scrot"),
        (["import", "-window", "root", str(output)], "ImageMagick import"),
    ]
    for command, executable in candidates:
        if shutil.which(command[0]) is None:
            continue
        result = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if result.returncode == 0 and output.is_file():
            return
        detail = result.stderr.decode(errors="replace").strip()
        if detail:
            last_error = f"{executable}: {detail}"
    else:
        raise RuntimeError(globals().get(
            "last_error", "no desktop screenshot tool found"))


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--android", action="store_true")
    source.add_argument("--desktop", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--device")
    args = parser.parse_args()
    try:
        if args.android:
            capture_android(args.output, args.adb, args.device)
        else:
            capture_desktop(args.output)
    except (OSError, RuntimeError) as error:
        print(f"voxel screenshot capture failed: {error}", file=sys.stderr)
        return 2
    print(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
