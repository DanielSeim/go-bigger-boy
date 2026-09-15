#!/usr/bin/env python3
"""Validate stable visual anchors in captured desktop debugger frames."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path.name}: expected binary PPM (P6)")
    index = 2

    def token() -> bytes:
        nonlocal index
        while index < len(data):
            if data[index:index + 1] == b"#":
                newline = data.find(b"\n", index)
                index = len(data) if newline < 0 else newline + 1
            elif data[index:index + 1].isspace():
                index += 1
            else:
                break
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        return data[start:index]

    width = int(token())
    height = int(token())
    maximum = int(token())
    if maximum != 255:
        raise ValueError(f"{path.name}: unsupported PPM maximum {maximum}")
    while index < len(data) and data[index:index + 1].isspace():
        index += 1
    pixels = data[index:]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path.name}: pixel payload has the wrong size")
    return width, height, pixels


def pixel(pixels: bytes, width: int, x: int, y: int) -> tuple[int, int, int]:
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset + 3])  # type: ignore[return-value]


def count_color(pixels: bytes, color: tuple[int, int, int]) -> int:
    return sum(
        1
        for offset in range(0, len(pixels), 3)
        if tuple(pixels[offset:offset + 3]) == color
    )


def validate(
    path: Path,
    expected_size: tuple[int, int],
    anchors: list[tuple[int, int, tuple[int, int, int]]],
) -> None:
    width, height, pixels = read_ppm(path)
    if (width, height) != expected_size:
        raise ValueError(
            f"{path.name}: expected {expected_size[0]}x{expected_size[1]}, "
            f"got {width}x{height}"
        )
    for x, y, color in anchors:
        actual = pixel(pixels, width, x, y)
        if actual != color:
            raise ValueError(
                f"{path.name}: anchor ({x},{y}) is {actual}, expected {color}"
            )
    background = count_color(pixels, (8, 12, 20))
    if background >= width * height * 0.98:
        raise ValueError(f"{path.name}: captured frame is effectively empty")
    print(f"validated {path.name}: {width}x{height}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    directory: Path = parser.parse_args().directory
    expected = {
        "debugger-normal-1280x900.ppm": (
            (1280, 900),
            [(24, 72, (69, 207, 238))],
        ),
        "debugger-normal-960x700.ppm": (
            (960, 700),
            [(24, 72, (69, 207, 238))],
        ),
        "debugger-inspector-960x700.ppm": (
            (960, 700),
            [(24, 64, (34, 91, 111)), (32, 366, (34, 91, 111))],
        ),
    }
    missing = [name for name in expected if not (directory / name).is_file()]
    if missing:
        print("missing debugger captures: " + ", ".join(missing),
              file=sys.stderr)
        return 1
    try:
        for name, (size, anchors) in expected.items():
            validate(directory / name, size, anchors)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("desktop debugger visual capture contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
