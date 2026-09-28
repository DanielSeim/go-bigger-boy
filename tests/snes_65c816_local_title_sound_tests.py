#!/usr/bin/env python3
"""Optional local-ROM test: unmodified Donkey Kong SOUND reaches host PCM."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys

DONKEY_SHA256 = "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4"
SCRIPT_SHA256 = "a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("program")
    parser.add_argument("ipl")
    parser.add_argument("gb_rom")
    parser.add_argument("gb_boot")
    parser.add_argument("input_script")
    args = parser.parse_args()
    if hashlib.sha256(Path(args.gb_rom).read_bytes()).hexdigest() != DONKEY_SHA256:
        raise AssertionError("local Donkey Kong ROM does not match the pinned title")
    if hashlib.sha256(Path(args.input_script).read_bytes()).hexdigest() != SCRIPT_SHA256:
        raise AssertionError("Donkey Kong input script does not match the pinned sequence")
    result = subprocess.run(
        [args.trace, args.program, args.ipl, "--sync-gb-sgb2",
         args.gb_rom, args.gb_boot, "--input-script", args.input_script,
         "--instruction-limit", "40000000"],
        capture_output=True, text=True, timeout=180, check=False,
    )
    output = result.stdout + result.stderr
    if result.returncode != 4 or "SNES CPU trace reached instruction bound" not in output:
        raise AssertionError(f"unexpected bounded trace outcome: {output[-3000:]}")
    if "first audible SOUND frame=2472 packet= 41 00 00 00 01" not in output:
        raise AssertionError("title-authentic music-score packet was not captured")
    delivered = re.search(r"audible_SOUND_delivered=(\d+)", output)
    nonzero = re.search(r"post_audible_SOUND_nonzero=(\d+)", output)
    if delivered is None or int(delivered.group(1)) == 0:
        raise AssertionError("audible title packet did not reach SNES host")
    if nonzero is None or int(nonzero.group(1)) == 0:
        raise AssertionError("no nonzero PCM after audible packet delivery")
    print("unmodified Donkey Kong title SOUND delivered to SNES host; "
          f"{nonzero.group(1)} nonzero PCM samples after delivery")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"local title SOUND test failed: {error}", file=sys.stderr)
        sys.exit(1)
