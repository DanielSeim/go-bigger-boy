#!/usr/bin/env python3
"""Validate clock-timed PMON, EON and FLG against independent S-DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_voice_register_pcm_tests import (
    all_voice_fixture, event, static_fixture,
)

EXPECTED_CORPUS_SHA256 = "46d6bcc9323f0afe61c8bbd06c5280b3cd9dafe1004eb3f5175cb7b6de039ce5"


def fixture_setup(timeline: Path) -> bytes:
    static = static_fixture(timeline)
    marker = b"clock 2048\n"
    if static.count(marker) != 1:
        raise AssertionError("two-voice fixture changed")
    return static.split(marker, 1)[0] + marker


def echo_setup(static: bytes, eon: int) -> bytes:
    old = b"reg 0x6c 0x20\n"
    if static.count(old) != 1:
        raise AssertionError("FLG setup changed")
    new = (f"reg 0x6c 0x00\nreg 0x4d {eon}\n"
           "reg 0x2c 0x7f\nreg 0x3c 0x7f\nreg 0x7f 0x7f\n").encode()
    return static.replace(old, new)


def cases(static: bytes) -> dict[str, tuple[bytes, bytes]]:
    all_static = all_voice_fixture(static)
    echo_route = echo_setup(static, 0)
    echo_write = echo_setup(static, 3)
    echo_enable = echo_write.replace(b"reg 0x6c 0x00\n",
                                     b"reg 0x6c 0x20\n")
    noise = static.replace(b"reg 0x6c 0x20\n",
                           b"reg 0x6c 0x00\nreg 0x3d 0x03\n")
    specs = {
        "pmon": (static, 0x2D, 0x02, 27),
        "pmon_all": (all_static, 0x2D, 0xFE, 27),
        "eon": (echo_route, 0x4D, 0x03, 28),
        "eon_all": (echo_setup(all_static, 0), 0x4D, 0xFF, 28),
        "mute": (static, 0x6C, 0x60, 27),
        "unmute": (static.replace(b"reg 0x6c 0x20\n",
                                   b"reg 0x6c 0x60\n"), 0x6C, 0x20, 27),
        "echo_left": (echo_write, 0x6C, 0x20, 28),
        "echo_right": (echo_write, 0x6C, 0x20, 29),
        "echo_enable_left": (echo_enable, 0x6C, 0x00, 28),
        "echo_enable_right": (echo_enable, 0x6C, 0x00, 29),
        "noise_rate": (noise, 0x6C, 0x1F, 30),
        "soft_reset": (static, 0x6C, 0xA0, 1),
        "soft_reset_v0": (static, 0x6C, 0xA0, 30),
    }
    return {name: (event(base, address, value, phase, 1024),
                   event(base, address, value, phase + 1, 1024))
            for name, (base, address, value, phase) in specs.items()}


def run(binary: Path, fixture: bytes) -> bytes:
    result = subprocess.run([str(binary)], input=fixture, capture_output=True,
                            check=True, timeout=30)
    if len(result.stdout) != (64 + 32) * 4:
        raise AssertionError(f"wrong PCM length from {binary}: {len(result.stdout)}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    static = fixture_setup(args.timeline)
    with tempfile.TemporaryDirectory(prefix="gbb-snes-dsp-global-") as temp:
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
            raise AssertionError(f"global-register PCM corpus changed: {corpus.hexdigest()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"timed global-register PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
