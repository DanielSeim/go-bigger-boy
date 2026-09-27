#!/usr/bin/env python3
"""Replay a synthetic SPC700 DSP-write timeline through both PCM engines.

The external DSP checkout is optional and used only in a local comparison.
The normal test pins the PCM output of GBB's development-only renderer.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile


def run(binary: Path, stimulus: bytes | None = None) -> bytes:
    result = subprocess.run([str(binary)] + (["--fixture"] if stimulus is None else []),
                            input=stimulus, capture_output=True, check=True,
                            timeout=30)
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeline", type=Path)
    parser.add_argument("gbb", type=Path)
    parser.add_argument("--reference-dir", type=Path,
                        help="independent SPC_DSP checkout, never shipped")
    args = parser.parse_args()

    stimulus = run(args.timeline)
    lines = stimulus.decode("ascii").splitlines()
    expected = [("step 1", "reg 12 127"),
                ("step 1", "reg 28 127"),
                ("step 1", "reg 76 1")]
    actual = [(lines[i - 1], lines[i]) for i, line in enumerate(lines)
              if line.startswith("reg ") and i > 0 and lines[i - 1].startswith("step ")]
    if actual != expected or lines[-1] != "step 125":
        raise AssertionError(f"unexpected SPC700 DSP write schedule: {actual}")

    ours = run(args.gbb, stimulus)
    if len(ours) != 128 * 4 or not any(ours):
        raise AssertionError("synthetic SPC700 timeline did not produce 128 audible samples")
    digest = hashlib.sha256(ours).hexdigest()
    # Pin the synthetic PCM on CI machines without the external source.
    expected_digest = "ff635a8ca007150e72ca7770897abebfeb86720841bec19f36b6f5ba7d796882"
    if digest != expected_digest:
        raise AssertionError(f"timeline PCM SHA-256 changed: {digest}")

    if args.reference_dir:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
        from compare_snes_dsp_pcm import build_reference, run_fixture
        with tempfile.TemporaryDirectory(prefix="gbb-spc-dsp-timeline-") as temp:
            reference = Path(temp) / "reference-dsp"
            build_reference(args.reference_dir, reference)
            result = run_fixture(reference, stimulus)
        reference_bytes = b"".join(
            int(channel).to_bytes(2, "little", signed=True)
            for sample in result for channel in sample)
        if ours != reference_bytes:
            first = next(i for i in range(128) if ours[4*i:4*i+4] !=
                         reference_bytes[4*i:4*i+4])
            raise AssertionError(f"reference PCM differs at stereo sample {first}")
        print("SPC700 timeline: 128 stereo samples match independent DSP exactly")
    print(f"SPC700 timeline PCM SHA-256: {digest}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.CalledProcessError) as error:
        print(f"timeline test failed: {error}", file=sys.stderr)
        sys.exit(1)
