#!/usr/bin/env python3
"""Optional regression check against locally supplied, unbundled SGB firmware."""

import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) not in (4, 5) or (len(sys.argv) == 5 and
                                    sys.argv[4] not in ("--boot", "--driver")):
        print("usage: script TRACE_EXECUTABLE PROGRAM_ROM SPC700_IPL "
              "[--boot|--driver]", file=sys.stderr)
        return 2
    boot = len(sys.argv) == 5
    driver = boot and sys.argv[4] == "--driver"
    result = subprocess.run(
        [sys.argv[1], sys.argv[2], sys.argv[3],
         "--driver-probe" if driver else "--upload-boot" if boot else "--upload"],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if result.returncode:
        print(result.stdout + result.stderr, file=sys.stderr)
        return result.returncode
    lines = result.stdout.strip().splitlines()
    expected_blocks = [
        ("first", "4c30", 378, "5e07f0679df67eb8"),
        ("second", "4c10", 24, "a1271438c0c58326"),
        ("third", "0400", 9971, "9fc9fc2e2007541a"),
        ("APU upload block 4", "4b00", 256, "c7307f52a5d3d038"),
        ("APU upload block 5", "4db0", 41280, "32c9e663a9075e9a"),
    ]
    if len(lines) != (7 if driver else 6 if boot else 1):
        print(f"unexpected upload line count: {lines!r}", file=sys.stderr)
        return 1
    for index, (label, destination, size, digest) in enumerate(
        expected_blocks if boot else expected_blocks[:1]
    ):
        prefix = f"{label} APU upload block" if index < 3 else label
        expected = (
            rf"{re.escape(prefix)}: destination \${destination} bytes={size} "
            rf"fnv64={digest} SNES_steps=(\d+) "
            r"master_clocks=(\d+) nmitimen=\$31 "
            r"nmi_ever_enabled=0 p=\$[0-9a-f]+"
        )
        match = re.fullmatch(expected, lines[index])
        if not match or int(match[1]) == 0 or int(match[2]) == 0:
            print(f"unexpected upload block {index + 1}: {lines[index]!r}",
                  file=sys.stderr)
            return 1
    if boot and not re.fullmatch(
        r"SPC700 entered uploaded program at \$0400 \(entry \$0400\) "
        r"after [1-9][0-9]* SNES steps", lines[5]
    ):
        print(f"unexpected SPC700 handoff: {lines[5]!r}", file=sys.stderr)
        return 1
    if driver and lines[6] != "first uploaded-driver DSP write: $4d=$00":
        print(f"unexpected uploaded-driver DSP write: {lines[6]!r}",
              file=sys.stderr)
        return 1
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
