#!/usr/bin/env python3
"""Compare SPC700-timed KON/KOFF polling windows with independent S-DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_subsample_pcm_tests import replace_once, run


EXPECTED_HASHES = {
    "kon": {
        "baseline": "b3f7ffb615b8681ad5142e8ded1395210d8bc746632ecf4e9e657b6219b2baa6",
        "early": "d5620235468612a4c9786bbd85139554b40121bd5fc88a147c91a18824b6f618",
        "late": "e4de0dbebbefc364424bafb646b58e6b39769c57978b014baade10ff85f5927e",
    },
    "koff": {
        "baseline": "44d0f1f16f2f270d9e125caa4937811bb660cf63610af6b46ab02dfa3cbe1efb",
        "early": "d0bb47fa77c8b971b4b6eb558cab00b1bbfec7206f93f9ed3d9cc339a474f034",
        "late": "4fad1351bce90bfec3581caa6177ee1f55141c5ab75472e5b151ea4f0eeb6996",
    },
}


def cases(baseline: bytes, address: int) -> dict[str, bytes]:
    middle = f"clock 33\nreg {address} 0\nclock 33".encode()
    second_before_poll = replace_once(
        baseline, middle, f"clock 36\nreg {address} 0\nclock 30".encode())
    second_at_poll = replace_once(
        baseline, middle, f"clock 37\nreg {address} 0\nclock 29".encode())
    second_after_poll = replace_once(
        baseline, middle, f"clock 38\nreg {address} 0\nclock 28".encode())
    tail = f"clock 33\nreg {address} 1\nclock 512".encode()
    third_before_poll = replace_once(
        baseline, tail, f"clock 67\nreg {address} 1\nclock 478".encode())
    third_at_poll = replace_once(
        baseline, tail, f"clock 68\nreg {address} 1\nclock 477".encode())
    third_after_poll = replace_once(
        baseline, tail, f"clock 69\nreg {address} 1\nclock 476".encode())
    post_poll_tail = f"clock 29\nreg {address} 1\nclock 512".encode()
    retrigger_before_clear = replace_once(
        second_at_poll, post_poll_tail,
        f"clock 62\nreg {address} 1\nclock 479".encode())
    retrigger_after_clear = replace_once(
        second_at_poll, post_poll_tail,
        f"clock 63\nreg {address} 1\nclock 478".encode())
    return {"baseline": baseline, "second_before_poll": second_before_poll,
            "second_at_poll": second_at_poll,
            "second_after_poll": second_after_poll,
            "third_before_poll": third_before_poll,
            "third_at_poll": third_at_poll,
            "third_after_poll": third_after_poll,
            "retrigger_before_clear": retrigger_before_clear,
            "retrigger_after_clear": retrigger_after_clear}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="gbb-spc-dsp-key-clock-") as temp:
        reference = None
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)

        for kind, address in (("kon", 76), ("koff", 92)):
            baseline = subprocess.check_output(
                [str(args.timeline), f"--{kind}-clock-fixture"], timeout=30)
            for fragment in (f"clock 26\nreg {address} 1".encode(),
                             f"clock 33\nreg {address} 0".encode(),
                             f"clock 33\nreg {address} 1".encode()):
                if fragment not in baseline:
                    raise AssertionError(f"{kind}: synthetic SPC700 event clocks changed")

            outputs: dict[str, bytes] = {}
            expected_class = {
                "baseline": "baseline",
                "second_before_poll": "baseline",
                "second_at_poll": "early",
                "second_after_poll": "early",
                "third_before_poll": "baseline",
                "third_at_poll": "late",
                "third_after_poll": "late",
                "retrigger_before_clear": "early",
                "retrigger_after_clear": "baseline" if kind == "kon" else "early",
            }
            for name, stimulus in cases(baseline, address).items():
                pcm = run(args.gbb, stimulus)
                if len(pcm) != 83 * 4:
                    raise AssertionError(f"{kind}/{name}: wrong stereo sample count")
                digest = hashlib.sha256(pcm).hexdigest()
                expected_digest = EXPECTED_HASHES[kind][expected_class[name]]
                if digest != expected_digest:
                    raise AssertionError(f"{kind}/{name}: PCM hash changed: {digest}")
                if reference:
                    expected = run(reference, stimulus)
                    if pcm != expected:
                        first = next(i for i in range(83) if
                                     pcm[4*i:4*i+4] != expected[4*i:4*i+4])
                        raise AssertionError(f"{kind}/{name}: reference differs at sample {first}")
                outputs[name] = pcm
                print(f"{kind}/{name}: {digest}")

            if outputs["baseline"] == bytes(len(outputs["baseline"])):
                raise AssertionError(f"{kind}: no audible PCM to compare")
            if outputs["second_at_poll"] == outputs["baseline"] or \
               outputs["third_at_poll"] == outputs["baseline"]:
                raise AssertionError(f"{kind}: phase-30 poll boundary did not affect PCM")
            if kind == "kon" and \
               outputs["retrigger_before_clear"] == outputs["retrigger_after_clear"]:
                raise AssertionError("KON phase-29 clear boundary did not affect PCM")
        if reference:
            print("all KON/KOFF clock traces match independent DSP exactly")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"key sub-sample PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
