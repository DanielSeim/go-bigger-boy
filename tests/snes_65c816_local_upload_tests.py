#!/usr/bin/env python3
"""Optional regression check against locally supplied, unbundled SGB firmware."""

import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: script TRACE_EXECUTABLE PROGRAM_ROM SPC700_IPL", file=sys.stderr)
        return 2
    result = subprocess.run(
        [sys.argv[1], sys.argv[2], sys.argv[3], "--upload"],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode:
        print(result.stdout + result.stderr, file=sys.stderr)
        return result.returncode
    line = result.stdout.strip()
    expected = (
        r"first APU upload block: destination \$4c30 bytes=378 "
        r"fnv64=5e07f0679df67eb8 SNES_steps=(\d+) "
        r"master_clocks=(\d+) nmitimen=\$31 "
        r"nmi_ever_enabled=0 p=\$35"
    )
    match = re.fullmatch(expected, line)
    if not match or int(match[1]) == 0 or int(match[2]) == 0:
        print(f"unexpected first-upload result: {line!r}", file=sys.stderr)
        return 1
    print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
