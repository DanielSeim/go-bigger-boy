#!/usr/bin/env python3
"""Run a visual test ROM and compare its framebuffer with a reference PNG."""

from __future__ import annotations

import argparse
import pathlib
import struct
import subprocess
import sys
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def read_ppm(path: pathlib.Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    position = 0

    def token() -> bytes:
        nonlocal position
        while position < len(data):
            if data[position] == ord("#"):
                position = data.find(b"\n", position)
                if position < 0:
                    raise ValueError("unterminated PPM comment")
            elif chr(data[position]).isspace():
                position += 1
            else:
                break
        end = position
        while end < len(data) and not chr(data[end]).isspace():
            end += 1
        result = data[position:end]
        position = end
        return result

    if token() != b"P6":
        raise ValueError("capture is not a binary PPM image")
    width = int(token())
    height = int(token())
    if int(token()) != 255:
        raise ValueError("only 8-bit PPM images are supported")
    if position >= len(data) or not chr(data[position]).isspace():
        raise ValueError("PPM header has no pixel-data separator")
    position += 2 if data[position : position + 2] == b"\r\n" else 1
    pixels = data[position:]
    if len(pixels) != width * height * 3:
        raise ValueError("PPM pixel data has the wrong size")
    return width, height, pixels


def paeth(left: int, above: int, upper_left: int) -> int:
    prediction = left + above - upper_left
    left_distance = abs(prediction - left)
    above_distance = abs(prediction - above)
    upper_left_distance = abs(prediction - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def unpack_samples(row: bytes, count: int, bit_depth: int) -> list[int]:
    if bit_depth == 8:
        return list(row[:count])
    if bit_depth == 16:
        return [struct.unpack_from(">H", row, index * 2)[0]
                for index in range(count)]
    mask = (1 << bit_depth) - 1
    samples = []
    for index in range(count):
        bit = index * bit_depth
        shift = 8 - bit_depth - (bit % 8)
        samples.append((row[bit // 8] >> shift) & mask)
    return samples


def read_png(path: pathlib.Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("reference is not a PNG image")

    position = len(PNG_SIGNATURE)
    header = None
    palette = None
    compressed = bytearray()
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        chunk_type = data[position + 4 : position + 8]
        chunk_data = data[position + 8 : position + 8 + length]
        position += 12 + length
        if chunk_type == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk_data)
        elif chunk_type == b"PLTE":
            palette = [tuple(chunk_data[index : index + 3])
                       for index in range(0, len(chunk_data), 3)]
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if header is None:
        raise ValueError("PNG has no IHDR chunk")
    width, height, bit_depth, color_type, compression, filtering, interlace = header
    if compression != 0 or filtering != 0 or interlace != 0:
        raise ValueError("only non-interlaced standard PNG images are supported")
    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_by_type:
        raise ValueError(f"unsupported PNG color type {color_type}")
    valid_depths = {0: {1, 2, 4, 8, 16}, 2: {8, 16},
                    3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
    if bit_depth not in valid_depths[color_type]:
        raise ValueError(f"unsupported bit depth {bit_depth} for PNG color type "
                         f"{color_type}")
    channels = channels_by_type[color_type]
    row_size = (width * channels * bit_depth + 7) // 8
    filter_bytes = max(1, (channels * bit_depth + 7) // 8)
    raw = zlib.decompress(bytes(compressed))
    if len(raw) != height * (row_size + 1):
        raise ValueError("PNG decompressed data has the wrong size")

    rows = []
    previous = bytes(row_size)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        encoded = raw[offset + 1 : offset + 1 + row_size]
        offset += row_size + 1
        decoded = bytearray(row_size)
        for index, value in enumerate(encoded):
            left = decoded[index - filter_bytes] if index >= filter_bytes else 0
            above = previous[index]
            upper_left = previous[index - filter_bytes] if index >= filter_bytes else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            elif filter_type == 4:
                predictor = paeth(left, above, upper_left)
            else:
                raise ValueError(f"unsupported PNG filter {filter_type}")
            decoded[index] = (value + predictor) & 0xFF
        rows.append(bytes(decoded))
        previous = bytes(decoded)

    rgb = bytearray()
    maximum = (1 << bit_depth) - 1
    for row in rows:
        samples = unpack_samples(row, width * channels, bit_depth)
        for x in range(width):
            pixel = samples[x * channels : (x + 1) * channels]
            if color_type == 0:
                gray = pixel[0] * 255 // maximum
                rgb.extend((gray, gray, gray))
            elif color_type == 2:
                rgb.extend(channel * 255 // maximum for channel in pixel[:3])
            elif color_type == 3:
                if palette is None or pixel[0] >= len(palette):
                    raise ValueError("PNG palette index is out of range")
                rgb.extend(palette[pixel[0]])
            elif color_type == 4:
                gray = pixel[0] * 255 // maximum
                rgb.extend((gray, gray, gray))
            else:
                rgb.extend(channel * 255 // maximum for channel in pixel[:3])
    return width, height, bytes(rgb)


def write_ppm(path: pathlib.Path, width: int, height: int, pixels: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + pixels)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--reference", required=True, type=pathlib.Path)
    parser.add_argument("--actual", required=True, type=pathlib.Path)
    capture = parser.add_mutually_exclusive_group()
    capture.add_argument("--frames", type=int)
    capture.add_argument("--frame-on-ld-bb", action="store_true")
    parser.add_argument("--max-cycles", type=int, default=100_000_000)
    parser.add_argument("--model", default="auto")
    parser.add_argument("--dmg-compatibility-colors", action="store_true")
    args = parser.parse_args()

    args.actual.parent.mkdir(parents=True, exist_ok=True)
    diff_path = args.actual.with_name(args.actual.stem + "-diff.ppm")
    diff_path.unlink(missing_ok=True)
    command = [str(args.runner), str(args.rom),
               "--frame-output", str(args.actual), "--max-cycles",
               str(args.max_cycles), "--model", args.model]
    if args.frame_on_ld_bb:
        command.append("--frame-on-ld-bb")
    else:
        command.extend(("--frames", str(args.frames or 60)))
    if args.dmg_compatibility_colors:
        command.append("--dmg-compatibility-colors")
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        return completed.returncode

    actual_width, actual_height, actual = read_ppm(args.actual)
    expected_width, expected_height, expected = read_png(args.reference)
    if (actual_width, actual_height) != (expected_width, expected_height):
        print(f"FAIL: captured {actual_width}x{actual_height}, expected "
              f"{expected_width}x{expected_height}", file=sys.stderr)
        return 1

    mismatches = []
    diff = bytearray(len(actual))
    for pixel in range(actual_width * actual_height):
        start = pixel * 3
        actual_pixel = actual[start : start + 3]
        expected_pixel = expected[start : start + 3]
        if actual_pixel != expected_pixel:
            mismatches.append(pixel)
            diff[start : start + 3] = b"\xff\x00\xff"
        else:
            diff[start : start + 3] = bytes(channel // 4 for channel in actual_pixel)

    if mismatches:
        first = mismatches[0]
        x = first % actual_width
        y = first // actual_width
        start = first * 3
        write_ppm(diff_path, actual_width, actual_height, bytes(diff))
        print(f"FAIL: {len(mismatches)} of {actual_width * actual_height} pixels "
              f"differ; first mismatch at ({x}, {y}): "
              f"actual={tuple(actual[start:start + 3])}, "
              f"expected={tuple(expected[start:start + 3])}", file=sys.stderr)
        print(f"Captured frame: {args.actual}", file=sys.stderr)
        print(f"Difference image: {diff_path}", file=sys.stderr)
        return 1

    print(f"PASS: framebuffer matches {args.reference}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error, zlib.error) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(2)
