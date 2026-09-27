#!/usr/bin/env python3
"""Local-only real IPL upload -> synthetic DSP voice PCM validation."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_global_register_pcm_tests import fixture_setup

EXPECTED_EVENT_CYCLE = 2702
EXPECTED_PCM_SHA256 = "84d69c40e87dc2258a6476f38de75640a9e09a995e2b348f74d87ca1890ca144"


def run(binary: Path, fixture: bytes) -> bytes:
    return subprocess.run([str(binary)], input=fixture, capture_output=True,
                          check=True, timeout=30).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("ipl", type=Path)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()

    output = subprocess.run([str(args.probe), str(args.ipl), "--upload-event"],
                            capture_output=True, check=True, timeout=30).stdout
    fields = output.split()
    if len(fields) != 3:
        raise AssertionError(f"bad IPL upload event: {output!r}")
    cycle, address, value = map(int, fields)
    if (cycle, address, value) != (EXPECTED_EVENT_CYCLE, 0x4C, 1):
        raise AssertionError(f"IPL upload event changed: {output!r}")

    static = fixture_setup(args.timeline)
    key_on = b"reg 0x4c 0x03\n"
    if static.count(key_on) != 1 or not static.endswith(b"clock 2048\n"):
        raise AssertionError("static two-voice fixture changed")
    static = static.replace(key_on, b"reg 0x4c 0x00\n")
    silent = run(args.gbb, static + b"clock 2048\n")
    dynamic = (f"clock {cycle - 2048}\nreg {address} {value}\n"
               f"clock {4096 - cycle}\n").encode("ascii")
    pcm = run(args.gbb, static + dynamic)
    if len(pcm) != 128 * 4 or any(silent) or not any(pcm):
        raise AssertionError("IPL-triggered PCM was missing or baseline was not silent")
    if any(pcm[:(cycle // 32) * 4]):
        raise AssertionError("PCM started before the IPL program's DSP write")
    digest = hashlib.sha256(pcm).hexdigest()
    if digest != EXPECTED_PCM_SHA256:
        raise AssertionError(f"IPL-triggered PCM changed: {digest}")
    print(f"real IPL upload DSP event clock: {cycle}")
    print(f"real IPL-triggered PCM SHA-256: {digest}")

    if args.reference_dir:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from compare_snes_dsp_pcm import build_reference
        with tempfile.TemporaryDirectory(prefix="gbb-real-ipl-pcm-") as temp:
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
            expected = run(reference, static + dynamic)
        if pcm != expected:
            first = next(i for i in range(128) if
                         pcm[4*i:4*i+4] != expected[4*i:4*i+4])
            raise AssertionError(f"independent DSP differs at sample {first}")
        print("real-IPL-triggered PCM matches independent DSP")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"real IPL PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
