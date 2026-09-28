#!/usr/bin/env python3
"""Synthetic SNES host + synchronized SPC700 -> timed DSP -> 32 kHz PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_global_register_pcm_tests import fixture_setup

EXPECTED_PCM_SHA256 = "8c06547b63af1a2f972dcf2b15298257b1675298d187737de4845ea2a11d8b1d"


def run(binary: Path, stimulus: bytes = b"") -> bytes:
    return subprocess.run([str(binary)], input=stimulus, capture_output=True,
                          check=True, timeout=30).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()

    event = run(args.host).split()
    if event != [b"2304", b"76", b"3"]:
        raise AssertionError(f"synchronized host event changed: {event!r}")
    cycle, address, value = map(int, event)

    setup = fixture_setup(args.timeline)
    key_on = b"reg 0x4c 0x03\n"
    if setup.count(key_on) != 1 or not setup.endswith(b"clock 2048\n"):
        raise AssertionError("static two-voice fixture changed")
    setup = setup.replace(key_on, b"reg 0x4c 0x00\n")
    silence = setup + b"clock 2048\n"
    stimulus = (setup + (f"clock {cycle - 2048}\nreg {address} {value}\n"
                         f"clock {4096 - cycle}\n").encode("ascii"))
    silent_pcm = run(args.renderer, silence)
    pcm = run(args.renderer, stimulus)
    if len(pcm) != 128 * 4 or any(silent_pcm) or not any(pcm):
        raise AssertionError("synchronized host event did not start 128-sample PCM")
    if any(pcm[:(cycle // 32) * 4]):
        raise AssertionError("PCM started before synchronized DSP write")
    digest = hashlib.sha256(pcm).hexdigest()
    if digest != EXPECTED_PCM_SHA256:
        raise AssertionError(f"synchronized PCM changed: {digest}")
    print(f"synchronized DSP cycle={cycle}, PCM SHA-256={digest}")

    if args.reference_dir:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from compare_snes_dsp_pcm import build_reference
        with tempfile.TemporaryDirectory(prefix="gbb-snes-sync-sound-") as temp:
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
            expected = run(reference, stimulus)
        if pcm != expected:
            first = next(i for i in range(128) if
                         pcm[4*i:4*i+4] != expected[4*i:4*i+4])
            raise AssertionError(f"independent DSP differs at sample {first}")
        print("synchronized host PCM matches independent DSP")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"synchronized host PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
