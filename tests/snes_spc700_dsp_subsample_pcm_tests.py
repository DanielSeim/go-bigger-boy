#!/usr/bin/env python3
"""Check SPC700-generated sub-sample master-volume writes against DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary: Path, stimulus: bytes) -> bytes:
    result = subprocess.run([str(binary)], input=stimulus, capture_output=True,
                            check=True, timeout=30)
    return result.stdout


def replace_once(source: bytes, old: bytes, new: bytes) -> bytes:
    if source.count(old) != 1:
        raise AssertionError(f"expected one timeline fragment: {old!r}")
    return source.replace(old, new, 1)


def cases(baseline: bytes, left: int = 12, right: int = 28) -> dict[str, bytes]:
    left_late = replace_once(
        baseline, f"clock 26\nreg {left} 127\nclock 33".encode(),
        f"clock 27\nreg {left} 127\nclock 32".encode())
    right_late = replace_once(
        baseline, f"clock 33\nreg {right} 127\nclock 33".encode(),
        f"clock 34\nreg {right} 127\nclock 32".encode())
    left_early = replace_once(
        baseline, f"clock 33\nreg {left} 0\nclock 256".encode(),
        f"clock 31\nreg {left} 0\nclock 258".encode())
    return {"baseline": baseline, "left_late": left_late,
            "right_late": right_late, "left_early": left_early}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path,
                        help="optional independent SPC_DSP source directory")
    args = parser.parse_args()
    baseline = subprocess.check_output([str(args.timeline), "--clock-fixture"],
                                       timeout=30)
    if not all(fragment in baseline for fragment in
               (b"clock 26\nreg 12 127", b"clock 33\nreg 28 127",
                b"clock 33\nreg 12 0")):
        raise AssertionError("synthetic SPC700 event clocks changed")

    reference = None
    expected_hashes = {
        "baseline": "52fdf1d8422f73fcaf287a486e447c6ba72f72d0c90c0c15aa046144b2496fa9",
        "left_late": "816d458a7eb6f056a3f6d7c776ab4900a7245635cb1f97854211300d6c8d5854",
        "right_late": "b88edec9ddf9cbcb1e3529fec952078a94dd47cf08f0a4eff6671575f02a3d9e",
        "left_early": "1b039799776666ab389b9e8f049fbc5d06fdd8c784f5d611d81299df53367ff3",
    }
    with tempfile.TemporaryDirectory(prefix="gbb-spc-dsp-subclock-") as temp:
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
        pcm: dict[str, bytes] = {}
        for name, fixture in cases(baseline).items():
            output = run(args.gbb, fixture)
            if len(output) != 75 * 4:
                raise AssertionError(f"{name}: expected 75 stereo samples, got {len(output)//4}")
            pcm[name] = output
            digest = hashlib.sha256(output).hexdigest()
            if digest != expected_hashes[name]:
                raise AssertionError(f"{name}: PCM hash changed: {digest}")
            if reference:
                theirs = run(reference, fixture)
            if reference and output != theirs:
                first = next(i for i in range(75)
                             if output[4*i:4*i+4] != theirs[4*i:4*i+4])
                raise AssertionError(f"{name}: independent DSP differs at sample {first}")
            print(f"{name}: {digest}")

        # Polls happen at phases 26 (left) and 27 (right). Crossing each poll
        # must change PCM at the corresponding output sample, not only later.
        for name, sample in (("left_late", 64), ("right_late", 65),
                             ("left_early", 66)):
            differences = [i for i in range(75) if
                           pcm[name][4*i:4*i+4] != pcm["baseline"][4*i:4*i+4]]
            if differences != [sample]:
                raise AssertionError(f"{name}: expected only sample {sample} to change; "
                                     f"got {differences}")
        if reference:
            print("all four clock traces match independent DSP exactly")

    # Mid-sample writes other than master volume have no validated phase model.
    invalid_cases = {
        "mid-sample KON": replace_once(baseline, b"clock 26\nreg 12 127",
                                       b"clock 26\nreg 76 1"),
        "boundary KON": replace_once(baseline, b"clock 2048\nclock 26",
                                     b"clock 2048\nreg 76 1\nclock 26"),
        "mixed step/clock": b"clock 32\nstep 1\n",
    }
    for name, invalid in invalid_cases.items():
        result = subprocess.run([str(args.gbb)], input=invalid, capture_output=True,
                                timeout=30)
        if result.returncode != 3 or b"unsupported DSP mode" not in result.stderr:
            raise AssertionError(f"{name} was not rejected")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"sub-sample PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
