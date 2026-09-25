#!/usr/bin/env python3
"""Summarize decoded SGB commands from opt-in diagnostic traces.

Only command records are read; no ROM data is needed or emitted. A title's
command usage is evidence of demand, not proof that its visual/audio behavior
is correct or that a command's implementation is complete.
"""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re


COMMANDS = {
    0x00: ("PAL01", "viewport"),
    0x01: ("PAL23", "viewport"),
    0x02: ("PAL03", "viewport"),
    0x03: ("PAL12", "viewport"),
    0x04: ("ATTR_BLK", "viewport"),
    0x05: ("ATTR_LIN", "viewport"),
    0x06: ("ATTR_DIV", "viewport"),
    0x07: ("ATTR_CHR", "viewport"),
    0x08: ("SOUND", "unimplemented: SNES audio"),
    0x09: ("SOU_TRN", "unimplemented: SNES audio"),
    0x0A: ("PAL_SET", "viewport"),
    0x0B: ("PAL_TRN", "viewport"),
    0x0C: ("ATRC_EN", "unimplemented: SNES host UI"),
    0x0D: ("TEST_EN", "unimplemented: SNES host control"),
    0x0E: ("ICON_EN", "unimplemented: SNES host UI"),
    0x0F: ("DATA_SND", "unimplemented: SNES host memory"),
    0x10: ("DATA_TRN", "unimplemented: SNES host memory"),
    0x11: ("MLT_REQ", "controller IDs and input"),
    0x12: ("JUMP", "unimplemented: SNES execution"),
    0x13: ("CHR_TRN", "border"),
    0x14: ("PCT_TRN", "border"),
    0x15: ("ATTR_TRN", "viewport"),
    0x16: ("ATTR_SET", "viewport"),
    0x17: ("MASK_EN", "viewport"),
    0x18: ("OBJ_TRN", "unimplemented: SNES sprites"),
    0x19: ("PAL_PRI", "unimplemented: SNES host palette override"),
}

COMMAND_LINE = re.compile(
    r"^command sequence=(\d+) command=0x([0-9a-fA-F]{1,2}) "
    r"bytes=(\d+) packet=([0-9a-fA-F]+)$"
)
MAX_TRACE_BYTES = 16 * 1024 * 1024


def count_commands(path: Path) -> Counter[int]:
    if path.stat().st_size > MAX_TRACE_BYTES:
        raise ValueError(f"{path}: trace exceeds 16 MiB")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "GBB SGB trace" or lines[-1] != "end":
        raise ValueError(f"{path}: not a complete GBB SGB trace")
    counts: Counter[int] = Counter()
    sequences: set[int] = set()
    for number, line in enumerate(lines, 1):
        if not line.startswith("command "):
            continue
        match = COMMAND_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"{path}:{number}: invalid command record")
        sequence, command, size, packet = match.groups()
        command_id = int(command, 16)
        if (int(size) < 16 or int(size) > 112 or int(size) % 16 != 0 or
                len(packet) != int(size) * 2):
            raise ValueError(f"{path}:{number}: invalid packet length")
        if (int(packet[:2], 16) >> 3) != command_id:
            raise ValueError(f"{path}:{number}: command and packet disagree")
        if int(sequence) in sequences:
            raise ValueError(f"{path}:{number}: duplicate command sequence")
        sequences.add(int(sequence))
        counts[command_id] += 1
    return counts


def report(paths: list[Path]) -> str:
    total: Counter[int] = Counter()
    output = []
    for path in paths:
        counts = count_commands(path)
        output.append(f"{path.name}: {sum(counts.values())} decoded commands")
        for command, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
            name, status = COMMANDS.get(command, ("UNKNOWN", "unimplemented: unknown"))
            output.append(f"  0x{command:02X} {name:<8} {count:>5}  {status}")
        total.update(counts)
    output.append("Total across captures:")
    output.append("command  count  status")
    for command, count in sorted(total.items(), key=lambda item: (-item[1], item[0])):
        name, status = COMMANDS.get(command, ("UNKNOWN", "unimplemented: unknown"))
        output.append(f"0x{command:02X} {name:<8} {count:>5}  {status}")
    if not total:
        output.append("No decoded commands; capture longer or check SGB mode.")
    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        print(report(args.traces))
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
