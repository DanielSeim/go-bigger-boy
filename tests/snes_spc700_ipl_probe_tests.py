#!/usr/bin/env python3
"""Exercise the local-only SPC700 IPL probe without any firmware bytes."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    probe = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="gbb-ipl-probe-") as temp:
        image = Path(temp) / "synthetic-ipl.bin"
        image.write_bytes(bytes((0x8F, 0xAA, 0xF4,
                                 0x8F, 0xBB, 0xF5)) + bytes(58))
        result = subprocess.run([str(probe), str(image)], capture_output=True,
                                timeout=30)
        if result.returncode != 0 or b"after 10 cycles" not in result.stdout:
            raise AssertionError(f"synthetic readiness failed: {result!r}")

        image.write_bytes(bytes((0xFF,)) + bytes(63))
        result = subprocess.run([str(probe), str(image)], capture_output=True,
                                timeout=30)
        if result.returncode != 3 or b"PC $ffc0: $ff" not in result.stderr:
            raise AssertionError(f"unsupported opcode not diagnosed: {result!r}")

        image.write_bytes(bytes(63))
        result = subprocess.run([str(probe), str(image)], capture_output=True,
                                timeout=30)
        if result.returncode != 2 or b"exactly 64 bytes" not in result.stderr:
            raise AssertionError(f"invalid IPL size not diagnosed: {result!r}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"SPC700 IPL probe test failed: {error}", file=sys.stderr)
        sys.exit(1)
