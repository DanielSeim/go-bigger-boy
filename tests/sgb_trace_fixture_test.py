#!/usr/bin/env python3
"""Run the deterministic, non-copyrighted SGB trace fixture through the CLI."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


MAX_CYCLES = "200000"
ROM_SIZE = 0x8000


def fail(message: str) -> None:
    raise AssertionError(message)


def load_fixture(path: Path) -> bytes:
    rom = bytearray(ROM_SIZE)
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            offset_text, values_text = line.split(":", 1)
            offset = int(offset_text.strip(), 16)
            values = bytes.fromhex(values_text)
        except ValueError as error:
            fail(f"{path}:{line_number}: invalid fixture line: {error}")
        if offset < 0 or offset + len(values) > ROM_SIZE:
            fail(f"{path}:{line_number}: fixture bytes exceed 32 KiB ROM")
        rom[offset : offset + len(values)] = values
    return bytes(rom)


def run(command: list[str], expected_returncode: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != expected_returncode:
        fail(
            "command failed with return code "
            f"{result.returncode}: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def normalized_trace(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def capture(runner: Path, rom: Path, trace: Path) -> None:
    result = run(
        [
            str(runner),
            str(rom),
            "--model",
            "sgb",
            "--max-cycles",
            MAX_CYCLES,
            "--sgb-trace",
            str(trace),
        ],
        expected_returncode=2,
    )
    if "TIMEOUT after reaching the cycle limit" not in (
        result.stdout + result.stderr
    ):
        fail("fixture capture did not reach its expected bounded cycle limit")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner", type=Path)
    parser.add_argument("diff", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("golden", type=Path)
    parser.add_argument("--record-golden", action="store_true")
    args = parser.parse_args()

    rom_bytes = load_fixture(args.fixture)
    with tempfile.TemporaryDirectory(prefix="gbb-sgb-trace-") as temporary:
        temporary_path = Path(temporary)
        rom = temporary_path / "trace-fixture.gb"
        first_trace = temporary_path / "first.trace"
        second_trace = temporary_path / "second.trace"
        rom.write_bytes(rom_bytes)
        capture(args.runner, rom, first_trace)

        if args.record_golden:
            args.golden.write_bytes(first_trace.read_bytes())
            return 0
        if not args.golden.is_file():
            fail(f"missing SGB trace golden file: {args.golden}")
        golden = normalized_trace(args.golden)
        if normalized_trace(first_trace) != golden:
            fail("SGB fixture capture differs from the checked-in golden trace")

        capture(args.runner, rom, second_trace)
        if normalized_trace(first_trace) != normalized_trace(second_trace):
            fail("repeated SGB fixture captures are not deterministic")

        replay = run(
            [
                str(args.runner),
                str(rom),
                "--model",
                "sgb",
                "--replay-sgb-trace",
                str(args.golden),
            ]
        )
        if "SGB replay passed" not in replay.stdout:
            fail("SGB fixture replay did not report success")

        run([str(args.diff), str(args.golden), str(first_trace)])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
