#!/usr/bin/env python3
"""Summarize decoded SGB commands from opt-in diagnostic traces.

Only command records are read; no ROM data is needed or emitted. A title's
command usage is evidence of demand, not proof that its visual/audio behavior
is correct or that a command's implementation is complete.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
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
    0x09: ("SOU_TRN", "transfer latched; SNES audio unimplemented"),
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


@dataclass(frozen=True)
class CommandRecord:
    sequence: int
    command: int
    packet: bytes


def read_commands(path: Path) -> list[CommandRecord]:
    """Read validated JOYP command records; screen-transfer data is separate."""
    if path.stat().st_size > MAX_TRACE_BYTES:
        raise ValueError(f"{path}: trace exceeds 16 MiB")
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "GBB SGB trace" or lines[-1] != "end":
        raise ValueError(f"{path}: not a complete GBB SGB trace")
    records: list[CommandRecord] = []
    sequences: set[int] = set()
    for number, line in enumerate(lines, 1):
        if not line.startswith("command "):
            continue
        match = COMMAND_LINE.fullmatch(line)
        if match is None:
            raise ValueError(f"{path}:{number}: invalid command record")
        sequence, command, size, packet = match.groups()
        command_id = int(command, 16)
        packet_size = int(size)
        if (packet_size < 16 or packet_size > 112 or packet_size % 16 != 0 or
                len(packet) != packet_size * 2):
            raise ValueError(f"{path}:{number}: invalid packet length")
        if (int(packet[:2], 16) >> 3) != command_id:
            raise ValueError(f"{path}:{number}: command and packet disagree")
        if int(sequence) in sequences:
            raise ValueError(f"{path}:{number}: duplicate command sequence")
        sequences.add(int(sequence))
        records.append(CommandRecord(int(sequence), command_id,
                                     bytes.fromhex(packet)))
    return records


def count_commands(path: Path) -> Counter[int]:
    return Counter(record.command for record in read_commands(path))


def describe_sound(record: CommandRecord) -> str:
    """Decode a request without claiming that GBB renders SNES audio."""
    if record.command == 0x09:
        return "SOU_TRN 4 KiB screen transfer requested (payload not in JOYP trace)"
    if record.command != 0x08:
        raise ValueError("not an SGB sound command")
    if len(record.packet) != 16:
        raise ValueError("SOUND must contain exactly one 16-byte packet")
    effect_a, effect_b, flags, score = record.packet[1:5]
    pitch_a, volume_a = flags & 3, (flags >> 2) & 3
    pitch_b, volume_b = (flags >> 4) & 3, (flags >> 6) & 3
    action = "mute SNES sound" if volume_a == 3 else "sound request"
    return (f"SOUND {action}: A=0x{effect_a:02x} pitch={pitch_a} "
            f"volume={volume_a}, B=0x{effect_b:02x} pitch={pitch_b} "
            f"volume={volume_b}, score=0x{score:02x}")


def report(paths: list[Path]) -> str:
    total: Counter[int] = Counter()
    output = []
    for path in paths:
        records = read_commands(path)
        counts = Counter(record.command for record in records)
        output.append(f"{path.name}: {sum(counts.values())} decoded commands")
        for command, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
            name, status = COMMANDS.get(command, ("UNKNOWN", "unimplemented: unknown"))
            output.append(f"  0x{command:02X} {name:<8} {count:>5}  {status}")
        sound_records = [record for record in records if record.command in (0x08, 0x09)]
        if sound_records:
            output.append("  SNES-side sound requests (not rendered by GBB):")
            for record in sound_records:
                output.append(f"    #{record.sequence}: {describe_sound(record)}")
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
