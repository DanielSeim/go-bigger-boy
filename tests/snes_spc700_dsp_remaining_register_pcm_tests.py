#!/usr/bin/env python3
"""Compare timed NON, DIR and echo-control writes with independent S-DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_global_register_pcm_tests import echo_setup, fixture_setup
from snes_spc700_dsp_voice_register_pcm_tests import event

EXPECTED_CORPUS_SHA256 = "4c15584e506f44e140b932cc085158ccf498cc33ea9921c9783d60f00b5e0e08"


def cases(static: bytes) -> dict[str, tuple[bytes, bytes]]:
    echo = echo_setup(static, 3)
    noise = static.replace(b"reg 0x6c 0x20\n",
                           b"reg 0x6c 0x1f\n")
    # The second directory points voice 0 at a different looping BRR block.
    directory = static.replace(
        b"clock 2048\n",
        b"ram 0x2900 0x00\nram 0x2901 0x82\n"
        b"ram 0x2902 0x00\nram 0x2903 0x82\n"
        b"clock 2080\nclock 25\nreg 0x4c 0x01\n")
    esa = echo.replace(
        b"clock 2048\n",
        b"ram 0x4000 0x00\nram 0x4001 0x40\n"
        b"ram 0x4002 0x00\nram 0x4003 0x20\nclock 2048\n")
    edl = echo.replace(
        b"clock 2048\n",
        b"ram 0x0004 0x00\nram 0x0005 0x40\n"
        b"ram 0x0006 0x00\nram 0x0007 0x20\nclock 2048\n")
    specs = {
        "non": (noise, 0x3D, 0x03, 28, 1024),
        "dir": (directory, 0x5D, 0x29, 3, 1031),
        "efb": (echo, 0x0D, 0x60, 26, 1024),
        "esa": (esa, 0x6D, 0x40, 29, 1024),
        "edl": (edl, 0x7D, 0x01, 29, 1024),
    }
    result = {
        name: (event(base, address, value, phase, duration),
               event(base, address, value, phase + 1, duration))
        for name, (base, address, value, phase, duration) in specs.items()
    }
    for tap, phase in enumerate((22, 23, 23, 24, 24, 24, 25, 25)):
        value = 0x20 if tap == 7 else 0x7f
        result[f"fir{tap}"] = (event(echo, 0x0F + tap * 0x10, value, phase, 1024),
                                event(echo, 0x0F + tap * 0x10, value, phase + 1, 1024))
    return result


def run(binary: Path, fixture: bytes) -> bytes:
    clocks = sum(int(line.split()[1]) for line in fixture.splitlines()
                 if line.startswith(b"clock "))
    if clocks % 32:
        raise AssertionError("fixture must end at a complete DSP sample")
    result = subprocess.run([str(binary)], input=fixture, capture_output=True,
                            check=True, timeout=30)
    if len(result.stdout) != clocks // 32 * 4:
        raise AssertionError(f"wrong PCM length from {binary}: {len(result.stdout)}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    static = fixture_setup(args.timeline)
    with tempfile.TemporaryDirectory(prefix="gbb-snes-dsp-remaining-") as temp:
        reference = None
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
        corpus = hashlib.sha256()
        for name, (before, after) in cases(static).items():
            ours_before, ours_after = run(args.gbb, before), run(args.gbb, after)
            if ours_before == ours_after:
                raise AssertionError(f"{name}: read boundary had no PCM effect")
            for suffix, fixture, ours in (("before", before, ours_before),
                                          ("after", after, ours_after)):
                corpus.update(f"{name}_{suffix}".encode() + b"\0")
                corpus.update(ours)
                if reference:
                    expected = run(reference, fixture)
                    if ours != expected:
                        first = next(i for i in range(len(ours) // 4)
                                     if ours[i*4:i*4+4] != expected[i*4:i*4+4])
                        raise AssertionError(
                            f"{name}_{suffix}: independent DSP differs at sample {first}: "
                            f"{ours[first*4:first*4+4].hex()} != "
                            f"{expected[first*4:first*4+4].hex()}")
                print(f"{name}_{suffix}: {hashlib.sha256(ours).hexdigest()}")
        if corpus.hexdigest() != EXPECTED_CORPUS_SHA256:
            raise AssertionError(f"remaining-register PCM corpus changed: {corpus.hexdigest()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"timed remaining-register PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
