#!/usr/bin/env python3
"""Host command -> SPC700 DSP-port write -> timed synthetic PCM contract."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_global_register_pcm_tests import fixture_setup

EXPECTED_EVENT_CYCLES = (2079, 3104)
EXPECTED_PCM_SHA256 = "2879f7142418900c77fed1751ee76d9c11eda0e5e3d320b44040a95165499f0d"


def run(binary: Path, stimulus: bytes) -> bytes:
    return subprocess.run([str(binary)], input=stimulus, capture_output=True,
                          check=True, timeout=30).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()

    output = subprocess.run([str(args.host), "--event"], capture_output=True,
                            check=True, timeout=30).stdout
    lines = output.splitlines()
    if len(lines) != 2 or any(len(line.split()) != 3 for line in lines):
        raise AssertionError(f"bad host event: {output!r}")
    events = [tuple(map(int, line.split())) for line in lines]
    if tuple(event[0] for event in events) != EXPECTED_EVENT_CYCLES or [
        event[1:] for event in events] != [(0x4C, 3), (0x5C, 3)]:
        raise AssertionError(f"unexpected host-command DSP write: {output!r}")

    static = fixture_setup(args.timeline)
    key_on = b"reg 0x4c 0x03\n"
    if static.count(key_on) != 1 or not static.endswith(b"clock 2048\n"):
        raise AssertionError("static two-voice fixture changed")
    static = static.replace(key_on, b"reg 0x4c 0x00\n")
    baseline = static + b"clock 2048\n"
    stimulus = static
    last_clock = 2048
    for cycle, address, value in events:
        stimulus += f"clock {cycle - last_clock}\nreg {address} {value}\n".encode("ascii")
        last_clock = cycle
    stimulus += f"clock {4096 - last_clock}\n".encode("ascii")
    kon_only = static + (f"clock {events[0][0] - 2048}\nreg 76 3\n"
                         f"clock {4096 - events[0][0]}\n").encode("ascii")

    silent = run(args.gbb, baseline)
    pcm = run(args.gbb, stimulus)
    if len(pcm) != 128 * 4 or any(silent):
        raise AssertionError("uncommanded fixture was not silent")
    if not any(pcm) or pcm == silent or any(pcm[:(events[0][0] // 32) * 4]):
        raise AssertionError("command did not start audio after its DSP write")
    if pcm == run(args.gbb, kon_only):
        raise AssertionError("host KOFF command did not change PCM")
    digest = hashlib.sha256(pcm).hexdigest()
    if digest != EXPECTED_PCM_SHA256:
        raise AssertionError(f"host-command PCM changed: {digest}")
    print(f"host DSP event clocks: {events[0][0]}, {events[1][0]}")
    print(f"host-command PCM SHA-256: {digest}")

    if args.reference_dir:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from compare_snes_dsp_pcm import build_reference
        with tempfile.TemporaryDirectory(prefix="gbb-snes-host-sound-") as temp:
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
            expected = run(reference, stimulus)
        if pcm != expected:
            first = next(i for i in range(128) if
                         pcm[4*i:4*i+4] != expected[4*i:4*i+4])
            raise AssertionError(f"independent DSP differs at sample {first}")
        print("host-command PCM matches independent DSP")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"host-command PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
