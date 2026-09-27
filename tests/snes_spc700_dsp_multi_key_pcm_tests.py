#!/usr/bin/env python3
"""Check clock-shifted multi-voice KON/KOFF PCM and ENDX against S-DSP."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_subsample_pcm_tests import replace_once


EXPECTED = {
    "baseline": (
        "7ae14027890c84a29b94c3cdc3ecec73e2754b4c0c6bdb7937c402b275e107fa",
        "4ef87103a196acecd961dea95a213b5495933215fbd73663195edc0f89f3a7e9"),
    "koff_late": (
        "68fc634b36f5109d8353e954ac829a058656a00d1b1f1cb51cb64ba7fa3829b7",
        "4ef87103a196acecd961dea95a213b5495933215fbd73663195edc0f89f3a7e9"),
    "kon_early": (
        "a91982e556a9738b39096c08aa07a558e4dca44f6fb3cc28686bdfbfd8664734",
        "e6300bed350a87b14c651e008bcf6cb67c207d2e28f70c8e56bb3f22f8638ab8"),
    "kon_late": (
        "9a820a57c53bf653ea26abd1fa058384017fc7230a05e19f3866d8312c3f509a",
        "8f16b93cef4633f988c9f55725cf0251ddc17ce5675be7d6568d0e35b030e883"),
}

EXPECTED_CLASS = {
    "baseline": "baseline",
    "koff_before_poll": "baseline",
    "koff_at_poll": "koff_late",
    "koff_after_poll": "koff_late",
    "kon_before_poll": "kon_early",
    "kon_at_poll": "kon_late",
    "kon_after_poll": "kon_late",
    "combined_at_poll": "baseline",
}


def cases(baseline: bytes) -> dict[str, bytes]:
    middle = b"clock 33\nreg 92 1\nclock 33"
    tail = b"clock 33\nreg 76 2\nclock 512"
    return {
        "baseline": baseline,
        "koff_before_poll": replace_once(
            baseline, middle, b"clock 36\nreg 92 1\nclock 30"),
        "koff_at_poll": replace_once(
            baseline, middle, b"clock 37\nreg 92 1\nclock 29"),
        "koff_after_poll": replace_once(
            baseline, middle, b"clock 38\nreg 92 1\nclock 28"),
        "kon_before_poll": replace_once(
            baseline, tail, b"clock 67\nreg 76 2\nclock 478"),
        "kon_at_poll": replace_once(
            baseline, tail, b"clock 68\nreg 76 2\nclock 477"),
        "kon_after_poll": replace_once(
            baseline, tail, b"clock 69\nreg 76 2\nclock 476"),
        "combined_at_poll": replace_once(
            replace_once(baseline, middle,
                         b"clock 36\nreg 92 1\nclock 1"),
            b"reg 76 2\nclock 512", b"reg 76 2\nclock 541"),
    }


def with_endx_probes(stimulus: bytes) -> bytes:
    clock = 0
    result: list[str] = []
    # Probe immediately before/after each of voice 0 and voice 1's V7 ENDX
    # publication phases (2 and 5), plus the 32-clock sample boundary.
    checkpoints = (2, 3, 5, 6, 32)
    for line in stimulus.decode("ascii").splitlines():
        if not line.startswith("clock "):
            result.append(line)
            continue
        remaining = int(line.split()[1])
        while remaining:
            position = clock % 32
            target = next(value for value in checkpoints if value > position)
            advance = min(remaining, target - position)
            result.append(f"clock {advance}")
            clock += advance
            remaining -= advance
            if (clock % 32 or 32) in checkpoints:
                result.append("endx")
    return ("\n".join(result) + "\n").encode("ascii")


def run(binary: Path, stimulus: bytes) -> tuple[bytes, bytes]:
    result = subprocess.run([str(binary)], input=stimulus, capture_output=True,
                            check=True, timeout=30)
    return result.stdout, result.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    baseline = subprocess.check_output(
        [str(args.timeline), "--multi-key-clock-fixture"], timeout=30)
    for fragment in (b"clock 26\nreg 76 2", b"clock 33\nreg 92 1",
                     b"clock 33\nreg 76 2"):
        if fragment not in baseline:
            raise AssertionError("SPC700 multi-voice event clocks changed")

    with tempfile.TemporaryDirectory(prefix="gbb-snes-dsp-multikey-") as temp:
        reference = None
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)

        observed: dict[str, tuple[bytes, bytes]] = {}
        for name, fixture in cases(baseline).items():
            stimulus = with_endx_probes(fixture)
            pcm, endx = run(args.gbb, stimulus)
            if len(pcm) != 83 * 4:
                raise AssertionError(f"{name}: wrong stereo sample count")
            lines = endx.splitlines()
            if len(lines) != 414 or any(not line.startswith(b"endx ") for line in lines):
                raise AssertionError(f"{name}: wrong ENDX probe count or format")
            digest = (hashlib.sha256(pcm).hexdigest(),
                      hashlib.sha256(endx).hexdigest())
            if digest != EXPECTED[EXPECTED_CLASS[name]]:
                raise AssertionError(f"{name}: PCM or ENDX hash changed: {digest}")
            if name == "baseline":
                transitions = [(i, int(value.split()[1])) for i, value in
                               enumerate(lines) if i == 0 or value != lines[i - 1]]
                if transitions != [(0, 0), (61, 1), (63, 3),
                                   (333, 1), (383, 3)]:
                    raise AssertionError(f"voice-staggered ENDX transitions changed: "
                                         f"{transitions}")
            if reference:
                expected_pcm, expected_endx = run(reference, stimulus)
                if pcm != expected_pcm:
                    first = next(i for i in range(83) if
                                 pcm[4*i:4*i+4] != expected_pcm[4*i:4*i+4])
                    raise AssertionError(f"{name}: PCM mismatch at sample {first}")
                if endx != expected_endx:
                    first = next(i for i, (a, b) in
                                 enumerate(zip(lines, expected_endx.splitlines()))
                                 if a != b)
                    raise AssertionError(f"{name}: ENDX mismatch at probe {first}: "
                                         f"{lines[first]!r} != "
                                         f"{expected_endx.splitlines()[first]!r}")
            observed[name] = pcm, endx
            print(name, *digest)
        if observed["koff_at_poll"][0] == observed["koff_before_poll"][0] or \
           observed["kon_at_poll"] == observed["kon_before_poll"]:
            raise AssertionError("multi-voice key poll boundaries did not affect output")
        if reference:
            print("multi-voice PCM and ENDX match the independent DSP")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"multi-voice DSP test failed: {error}", file=sys.stderr)
        sys.exit(1)
