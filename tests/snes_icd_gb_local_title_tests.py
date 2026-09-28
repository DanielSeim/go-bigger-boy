#!/usr/bin/env python3
"""Pin an unmodified local SGB2 title's first audible music-score packet."""

import hashlib
from pathlib import Path
import re
import subprocess
import sys


DONKEY_SHA256 = "b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4"
SCRIPT_SHA256 = "a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b"


def main() -> int:
    if len(sys.argv) != 5:
        return 2
    runner, rom, boot, script = map(Path, sys.argv[1:])
    if hashlib.sha256(rom.read_bytes()).hexdigest() != DONKEY_SHA256:
        raise AssertionError("local Donkey Kong ROM does not match the pinned title")
    if hashlib.sha256(script.read_bytes()).hexdigest() != SCRIPT_SHA256:
        raise AssertionError("Donkey Kong input script does not match the pinned sequence")
    result = subprocess.run(
        [str(runner), str(rom), str(boot), str(script)],
        capture_output=True, text=True, timeout=120, check=False,
    )
    match = re.search(
        r"first audible SOUND frame=(\d+) packet= 41 00 00 00 01"
        r"(?: [0-9a-f]{2}){11} input_events=(\d+) SOU_TRN=(\d+)",
        result.stdout,
    )
    if result.returncode != 0 or match is None or \
            int(match.group(2)) < 2 or int(match.group(3)) < 2:
        raise AssertionError(f"title packet capture changed: {result}")
    print(f"unmodified Donkey Kong first music-score SOUND at GB frame {match.group(1)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"local SGB2 title packet test failed: {error}", file=sys.stderr)
        sys.exit(1)
