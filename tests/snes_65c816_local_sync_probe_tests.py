#!/usr/bin/env python3
"""Check bounded real-firmware synchronized diagnostics without bundling ROMs."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("program", type=Path)
    parser.add_argument("ipl", type=Path)
    parser.add_argument("kind", choices=("sgb1", "sgb2"))
    args = parser.parse_args()
    result = subprocess.run([str(args.trace), str(args.program), str(args.ipl),
                             "--sync-probe"], capture_output=True, text=True,
                            timeout=30)
    if result.returncode != 3 or "SPC700 entered uploaded program at $0400" not in result.stdout:
        raise AssertionError(f"unexpected synchronized handoff: {result}")
    if "post-handoff SPC RAM writes=" not in result.stdout or "fnv64=" not in result.stdout:
        raise AssertionError("synchronized RAM write diagnostic is missing")
    if args.kind == "sgb1":
        if "unsupported I/O/mapping access $006000" not in result.stderr:
            raise AssertionError("SGB1 did not fail closed at its first ICD read")
    else:
        events = [line for line in result.stdout.splitlines()
                  if line.startswith("synchronized DSP write")]
        if len(events) != 16 or "register $4d=$00" not in events[0] or \
                "register $7d=$02" not in events[-1] or \
                "post-handoff DSP writes=755 fnv64=d8ae50866eb3f926" not in result.stdout or \
                "unsupported I/O/mapping access $006000" not in result.stderr:
            raise AssertionError("SGB2 initialized DSP differently or did not trap")
    print(f"{args.kind}: synchronized firmware probe stopped explicitly")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"local synchronized probe failed: {error}", file=sys.stderr)
        sys.exit(1)
