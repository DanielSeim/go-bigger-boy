#!/usr/bin/env python3
"""Exercise the local-only SPC700 IPL probe without any firmware bytes."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile


def synthetic_uploader() -> bytes:
    # Original test program implementing one eight-byte transfer and entry.
    # It is deliberately distinct from any hardware IPL image.
    code = bytearray()
    labels: dict[str, int] = {}
    branches: list[tuple[int, str]] = []

    def emit(*values: int) -> None:
        code.extend(values)

    def mark(name: str) -> None:
        labels[name] = len(code)

    def branch(opcode: int, target: str) -> None:
        emit(opcode, 0)
        branches.append((len(code) - 1, target))

    emit(0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5)  # ready
    mark("wait")
    emit(0x78, 0xCC, 0xF4)
    branch(0xD0, "wait")
    mark("main")
    emit(0xBA, 0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4,
         0xDD, 0x5D)
    branch(0xD0, "transfer")
    emit(0x1F, 0x00, 0x00)  # jump through the uploaded entry pointer
    mark("transfer")
    emit(0x8F, 0x00, 0x02, 0xEB, 0x02, 0x8F, 0x08, 0x03)
    mark("wait_index")
    emit(0x7E, 0xF4)
    branch(0xD0, "wait_index")
    emit(0xE4, 0xF5, 0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0x7E, 0x03)
    branch(0xD0, "wait_index")
    branch(0x2F, "main")
    for offset, target in branches:
        displacement = labels[target] - (offset + 1)
        if not -128 <= displacement <= 127:
            raise AssertionError("synthetic IPL branch out of range")
        code[offset] = displacement & 0xFF
    if len(code) > 64:
        raise AssertionError("synthetic IPL too large")
    return bytes(code) + bytes(64 - len(code))


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

        image.write_bytes(synthetic_uploader())
        result = subprocess.run([str(probe), str(image), "--upload-smoke"],
                                capture_output=True, timeout=30)
        if result.returncode != 0 or b"DSP KON write succeeded" not in result.stdout:
            raise AssertionError(f"synthetic upload failed: {result!r}")
        result = subprocess.run([str(probe), str(image), "--upload-event"],
                                capture_output=True, timeout=30)
        event = result.stdout.split()
        if result.returncode != 0 or len(event) != 3 or event[1:] != [b"76", b"1"]:
            raise AssertionError(f"synthetic upload event failed: {result!r}")

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
