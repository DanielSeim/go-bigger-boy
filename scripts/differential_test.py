#!/usr/bin/env python3
"""Compare two runner-compatible emulators on one ROM.

The executables may be native GBB builds or small adapter wrappers around a
reference emulator.  Adapters should accept the runner options used below and
write the requested PPM/trace artifacts to the supplied paths.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from compare_frame import read_ppm, write_ppm  # noqa: E402


def result_marker(output: str) -> str:
    for marker in ("PASS (", "FAIL (", "TIMEOUT", "Error:",
                   "Captured LD B,B framebuffer"):
        for line in output.splitlines():
            if marker in line:
                return marker if marker.startswith("Captured ") \
                    else line[line.index(marker):].strip()
    return output.splitlines()[-1].strip() if output.splitlines() else ""


def command_for(executable: Path, rom: Path, model: str, max_cycles: int,
                frame: Path | None, trace: Path | None,
                trace_kind: str | None) -> list[str]:
    command = [str(executable), str(rom), "--max-cycles", str(max_cycles),
               "--model", model]
    if frame is not None:
        command.extend(("--frame-on-ld-bb", "--frame-output", str(frame)))
    else:
        command.extend(("--protocol", "mooneye"))
    if trace is not None and trace_kind is not None:
        command.extend((f"--trace-{trace_kind}", str(trace)))
    return command


def run(executable: Path, command: list[str], log: Path) -> int:
    completed = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, check=False,
    )
    log.write_text(completed.stdout or "", encoding="utf-8")
    return completed.returncode


def compare_frames(candidate: Path, reference: Path, diff: Path) -> str | None:
    candidate_width, candidate_height, candidate_pixels = read_ppm(candidate)
    reference_width, reference_height, reference_pixels = read_ppm(reference)
    if (candidate_width, candidate_height) != (reference_width, reference_height):
        return (f"frame dimensions differ: candidate={candidate_width}x"
                f"{candidate_height}, reference={reference_width}x"
                f"{reference_height}")
    if candidate_pixels == reference_pixels:
        return None
    mismatches = []
    pixels = bytearray(len(candidate_pixels))
    for index in range(0, len(candidate_pixels), 3):
        actual = candidate_pixels[index:index + 3]
        expected = reference_pixels[index:index + 3]
        if actual != expected:
            mismatches.append(index // 3)
            pixels[index:index + 3] = b"\xff\x00\xff"
        else:
            pixels[index:index + 3] = bytes(channel // 4 for channel in actual)
    write_ppm(diff, candidate_width, candidate_height, bytes(pixels))
    first = mismatches[0]
    return (f"{len(mismatches)} pixels differ; first at "
            f"({first % candidate_width}, {first // candidate_width}); "
            f"diff={diff}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--model", default="cgb-e")
    parser.add_argument("--max-cycles", type=int, default=100_000_000)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--frame", action="store_true",
                        help="compare the framebuffer captured at LD B,B")
    trace = parser.add_mutually_exclusive_group()
    trace.add_argument("--trace-apu", action="store_true")
    trace.add_argument("--trace-ppu", action="store_true")
    args = parser.parse_args()
    if args.max_cycles <= 0:
        parser.error("--max-cycles must be positive")

    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    trace_kind = "apu" if args.trace_apu else "ppu" if args.trace_ppu else None
    candidate_frame = output / "candidate.ppm" if args.frame else None
    reference_frame = output / "reference.ppm" if args.frame else None
    candidate_trace = output / "candidate.trace" if trace_kind else None
    reference_trace = output / "reference.trace" if trace_kind else None
    candidate_log = output / "candidate.log"
    reference_log = output / "reference.log"
    candidate_code = run(
        args.candidate,
        command_for(args.candidate, args.rom, args.model, args.max_cycles,
                    candidate_frame, candidate_trace, trace_kind),
        candidate_log,
    )
    reference_code = run(
        args.reference,
        command_for(args.reference, args.rom, args.model, args.max_cycles,
                    reference_frame, reference_trace, trace_kind),
        reference_log,
    )

    candidate_result = result_marker(candidate_log.read_text(encoding="utf-8"))
    reference_result = result_marker(reference_log.read_text(encoding="utf-8"))
    if candidate_code != reference_code or candidate_result != reference_result:
        print("FAIL: emulator results differ")
        print(f"candidate: exit={candidate_code} result={candidate_result}")
        print(f"reference: exit={reference_code} result={reference_result}")
        return 1
    if args.frame:
        diff = output / "frame-diff.ppm"
        try:
            mismatch = compare_frames(candidate_frame, reference_frame, diff)
        except (OSError, ValueError, struct.error) as error:
            print(f"FAIL: could not compare frames: {error}")
            return 2
        if mismatch is not None:
            print(f"FAIL: {mismatch}")
            return 1
    if trace_kind and candidate_trace.read_bytes() != reference_trace.read_bytes():
        print(f"FAIL: {trace_kind} traces differ")
        print(f"candidate trace: {candidate_trace}")
        print(f"reference trace: {reference_trace}")
        return 1
    print("PASS: emulator results match")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(2)
