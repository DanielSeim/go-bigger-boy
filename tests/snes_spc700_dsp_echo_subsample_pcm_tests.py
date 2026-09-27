#!/usr/bin/env python3
"""Compare clock-timed SPC700 echo-output volume writes with S-DSP PCM."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from snes_spc700_dsp_subsample_pcm_tests import cases, replace_once, run


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path)
    args = parser.parse_args()
    baseline = subprocess.check_output([str(args.timeline), "--echo-clock-fixture"],
                                       timeout=30)
    if not all(fragment in baseline for fragment in
               (b"clock 26\nreg 44 127", b"clock 33\nreg 60 127",
                b"clock 33\nreg 44 0")):
        raise AssertionError("synthetic SPC700 echo-volume event clocks changed")

    hashes = {
        "baseline": "a4544eb71dc00212b0df8830575ed6750e06e9d973db1a6b69ee0df94df7f50f",
        "left_late": "c412e28a41291044fcb117a4c4bfc173a3c86c2ad7daf66e29f5d33734122024",
        "right_late": "7c38e00c871158eb579d52bb8263c52bbe96c443497a078ed94eb9b8c48c5284",
        "left_early": "5b2cde1e172c233441ad11b02ebfbaa909aa20d0cb4f52c0e0dab6b28c3f516c",
    }
    with tempfile.TemporaryDirectory(prefix="gbb-spc-dsp-echo-clock-") as temp:
        reference = None
        if args.reference_dir:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
            from compare_snes_dsp_pcm import build_reference
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)

        output = {}
        for name, stimulus in cases(baseline, 44, 60).items():
            pcm = run(args.gbb, stimulus)
            if len(pcm) != 75 * 4:
                raise AssertionError(f"{name}: wrong stereo sample count")
            digest = hashlib.sha256(pcm).hexdigest()
            if digest != hashes[name]:
                raise AssertionError(f"{name}: PCM hash changed: {digest}")
            if reference:
                expected = run(reference, stimulus)
                if pcm != expected:
                    first = next(i for i in range(75) if
                                 pcm[4*i:4*i+4] != expected[4*i:4*i+4])
                    raise AssertionError(f"{name}: reference differs at sample {first}")
            output[name] = pcm
            print(f"{name}: {digest}")

        for name, target in (("left_late", 64), ("right_late", 65),
                             ("left_early", 66)):
            differences = [i for i in range(75) if
                           output[name][4*i:4*i+4] != output["baseline"][4*i:4*i+4]]
            if differences != [target]:
                raise AssertionError(f"{name}: expected only sample {target} to change, "
                                     f"got {differences}")
        if reference:
            print("all four echo-volume traces match independent DSP exactly")

    # Echo feedback is sampled elsewhere within the DSP cycle and is not yet
    # modeled by the clock fixture's output-volume latches.
    invalid = replace_once(baseline, b"clock 26\nreg 44 127",
                           b"clock 26\nreg 13 127")
    rejected = subprocess.run([str(args.gbb)], input=invalid, capture_output=True,
                              timeout=30)
    if rejected.returncode != 3 or b"unsupported DSP mode" not in rejected.stderr:
        raise AssertionError("timed echo-feedback write was not rejected")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"echo sub-sample PCM test failed: {error}", file=sys.stderr)
        sys.exit(1)
