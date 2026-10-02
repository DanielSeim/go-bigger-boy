#!/usr/bin/env python3
"""Compare identical synthetic DSP stimuli against an external reference.

The reference source is compiled into a temporary executable. It is never
copied into the repository or linked into emulator/release targets.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SUPPORT = ROOT / "tests" / "support"


def build_reference(source_dir: Path, output: Path) -> None:
    source = source_dir / "SPC_DSP.cpp"
    header = source_dir / "SPC_DSP.h"
    if not source.is_file() or not header.is_file():
        raise ValueError(f"expected SPC_DSP.cpp and SPC_DSP.h in {source_dir}")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    command = compiler + [
        "-std=c++17", "-O2", "-I", str(source_dir), "-I", str(SUPPORT),
        "-include", str(SUPPORT / "snes_dsp_reference_config.hpp"),
        str(SUPPORT / "snes_dsp_reference_fixture_runner.cpp"),
        str(source), "-o", str(output),
    ]
    if "diagnostic_state(" in header.read_text(encoding="utf-8"):
        command.insert(len(compiler), "-DGBB_REFERENCE_DIAGNOSTIC_STATE")
    if "diagnostic_key_poll_clock(" in header.read_text(encoding="utf-8"):
        command.insert(len(compiler), "-DGBB_REFERENCE_KEY_CLOCK")
    subprocess.run(command, check=True)


def run_fixture(binary: Path, fixture: bytes) -> list[tuple[int, int]]:
    result = subprocess.run([str(binary)], input=fixture, capture_output=True,
                            timeout=60)
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"{binary.name} failed ({result.returncode}): {detail}")
    if len(result.stdout) % 4:
        raise RuntimeError(f"{binary.name} returned incomplete stereo PCM")
    return list(struct.iter_unpack("<hh", result.stdout))


def first_mismatch(ours: list[tuple[int, int]], theirs: list[tuple[int, int]]
                   ) -> tuple[int, tuple[int, int], tuple[int, int]] | None:
    return next(((i, a, b) for i, (a, b) in enumerate(zip(ours, theirs))
                 if a != b), None)


def compare(fixture: Path, gbb: Path, reference: Path, max_differences: int) -> bool:
    stimulus = fixture.read_bytes()
    ours = run_fixture(gbb, stimulus)
    theirs = run_fixture(reference, stimulus)
    if len(ours) != len(theirs):
        print(f"{fixture.name}: frame count differs: GBB={len(ours)}, "
              f"reference={len(theirs)}")
        return False
    differences = [(index, a, b) for index, (a, b) in
                   enumerate(zip(ours, theirs)) if a != b]
    if differences:
        print(f"{fixture.name}: {len(differences)}/{len(ours)} stereo samples "
              "differ (exact 32 kHz alignment)")
        first = first_mismatch(ours, theirs)
        assert first is not None
        print(f"  first mismatch: sample {first[0]}")
        first_ours = next((i for i, sample in enumerate(ours) if sample != (0, 0)), None)
        first_reference = next((i for i, sample in enumerate(theirs)
                                if sample != (0, 0)), None)
        if first_ours is not None and first_reference is not None:
            print(f"  first nonzero: GBB={first_ours}, reference={first_reference} "
                  f"(reference minus GBB={first_reference - first_ours})")
        for index, a, b in differences[:max_differences]:
            print(f"  sample {index}: GBB={a}, reference={b}, "
                  f"delta=({a[0] - b[0]}, {a[1] - b[1]})")
        return False
    print(f"{fixture.name}: {len(ours)} stereo samples match exactly")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gbb", type=Path, required=True,
                        help="built gameboy_snes_dsp_pcm_fixture_runner")
    parser.add_argument("--reference-dir", type=Path, required=True,
                        help="external checkout directory containing SPC_DSP.cpp")
    parser.add_argument("fixtures", type=Path, nargs="+",
                        help="shared text stimulus files")
    parser.add_argument("--max-differences", type=int, default=10)
    args = parser.parse_args()
    if args.max_differences < 1:
        parser.error("--max-differences must be positive")
    if not args.gbb.is_file():
        parser.error(f"GBB runner does not exist: {args.gbb}")
    try:
        with tempfile.TemporaryDirectory(prefix="gbb-snes-dsp-diff-") as temporary:
            reference = Path(temporary) / "reference-dsp"
            build_reference(args.reference_dir, reference)
            results = [compare(path, args.gbb.resolve(), reference,
                               args.max_differences)
                       for path in args.fixtures]
        return 0 if all(results) else 1
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"comparison failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
