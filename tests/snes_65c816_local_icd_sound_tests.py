#!/usr/bin/env python3
"""Optional local-ROM ICD-to-host SOUND handoff; not an audible-sound test."""

import argparse
import re
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("program")
    parser.add_argument("ipl")
    parser.add_argument("gb_rom")
    parser.add_argument("gb_boot")
    args = parser.parse_args()
    result = subprocess.run(
        [args.trace, args.program, args.ipl, "--sync-gb-sgb2",
         args.gb_rom, args.gb_boot],
        capture_output=True, text=True, timeout=120, check=False,
    )
    output = result.stdout + result.stderr
    if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in output:
        raise AssertionError(f"unexpected bounded trace outcome: {output[-2000:]}")
    if "SOUND=1" not in output or "SOUND_delivered=1" not in output:
        raise AssertionError("GB SOUND packet did not reach the SNES ICD reader")
    if "first SOUND packet= 41 80 80 8c 00" not in output:
        raise AssertionError("unexpected Donkey Kong SOUND packet")
    if "PCM_nonzero=0" in output:
        raise AssertionError("APU initialization PCM observation was silent")
    if "post_SOUND_nonzero=0" not in output:
        raise AssertionError("mute command unexpectedly produced nonzero PCM")

    substituted = subprocess.run(
        [args.trace, args.program, args.ipl, "--sync-gb-sgb2",
         args.gb_rom, args.gb_boot, "--audible-sound-probe"],
        capture_output=True, text=True, timeout=120, check=False,
    )
    synthetic_output = substituted.stdout + substituted.stderr
    samples = re.search(r"post_SOUND_nonzero=(\d+)", synthetic_output)
    if substituted.returncode != 3 or \
            "SYNTHETIC SOUND payload substitution enabled" not in synthetic_output or \
            "SOUND_delivered=1" not in synthetic_output or \
            "first SOUND packet= 41 03 80 00 00" not in synthetic_output or \
            samples is None or int(samples.group(1)) == 0:
        raise AssertionError(f"synthetic audible handoff failed: {synthetic_output[-2000:]}")
    print("real GB mute packet reached host and remained silent; explicitly "
          f"substituted audible packet yielded {samples.group(1)} nonzero PCM samples")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"local ICD SOUND handoff failed: {error}", file=sys.stderr)
        sys.exit(1)
