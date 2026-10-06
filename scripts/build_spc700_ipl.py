#!/usr/bin/env python3
"""Assemble original GBB IPL with a small, strict SPC700 instruction subset."""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/spc700/ipl.asm"
HEADER = ROOT / "firmware/spc700/ipl_image.hpp"


def assemble(source):
    # Operand patterns encode SPC700 instructions, never reference firmware.
    forms = {
        "dec x": (0x1d,),
        "mov x, #@": (0xcd,), "mov sp, x": (0xbd,),
        "mov a, #@": (0xe8,),
        "mov (x), a": (0xc6,),
        "mov $f4, #@": (0x8f, None, 0xf4),
        "mov $f5, #@": (0x8f, None, 0xf5),
        "cmp $f4, #@": (0x78, None, 0xf4),
        "movw ya, $f6": (0xba, 0xf6), "movw $00, ya": (0xda, 0),
        "movw ya, $f4": (0xba, 0xf4), "mov a, y": (0xdd,), "mov x, a": (0x5d,),
        "mov $f4, a": (0xc4, 0xf4),
        "mov y, $f4": (0xeb, 0xf4),
        "cmp y, $f4": (0x7e, 0xf4), "mov a, $f5": (0xe4, 0xf5),
        "mov [$00]+y, a": (0xd7, 0), "mov $f4, y": (0xcb, 0xf4),
        "inc y": (0xfc,), "inc $01": (0xab, 1),
        "jmp [$0000+x]": (0x1f, 0, 0),
    }
    branches = {"bne": 0xd0, "bpl": 0x10, "bra": 0x2f}
    code, labels, fixups = bytearray(), {}, []
    for line in source.splitlines():
        instruction = line.split(";", 1)[0].strip().lower()
        if not instruction:
            continue
        if instruction.endswith(":"):
            name = instruction[:-1]
            if name in labels:
                raise ValueError(f"duplicate label {name}")
            labels[name] = len(code)
            continue
        mnemonic, _, operand = instruction.partition(" ")
        if mnemonic in branches:
            code.extend((branches[mnemonic], 0))
            fixups.append((len(code) - 1, operand))
            continue
        immediate = re.search(r"#\$([0-9a-f]{2})$", instruction)
        pattern = re.sub(r"#\$[0-9a-f]{2}$", "#@", instruction)
        if pattern not in forms:
            raise ValueError(f"unsupported instruction: {instruction}")
        encoding = forms[pattern]
        if immediate:
            value = int(immediate[1], 16)
            encoding = tuple(value if v is None else v for v in encoding)
            if None not in forms[pattern]:
                encoding += (value,)
        code.extend(encoding)
    for offset, target in fixups:
        distance = labels[target] - offset - 1
        if not -128 <= distance <= 127:
            raise ValueError(f"branch out of range: {target}")
        code[offset] = distance & 255
    if len(code) > 64:
        raise ValueError(f"IPL exceeds 64 bytes: {len(code)}")
    code.extend(bytes(64 - len(code))) # Unreachable NOP padding.
    return bytes(code)


def render(source, image):
    rows = ["    " + ", ".join(f"0x{b:02X}" for b in image[i:i+16]) + ","
            for i in range(0, 64, 16)]
    return ("// SPDX-License-Identifier: GPL-3.0-or-later\n"
            "// Generated from ipl.asm by scripts/build_spc700_ipl.py.\n"
            f"// Source SHA-256 (LF): {hashlib.sha256(source.encode()).hexdigest()}\n"
            f"// Image SHA-256: {hashlib.sha256(image).hexdigest()}\n"
            "#pragma once\n#include <array>\n#include <cstdint>\n\n"
            "namespace gameboy::firmware {\n"
            "inline constexpr std::array<std::uint8_t, 64> spc700_ipl_image{\n"
            + "\n".join(rows) + "\n};\n} // namespace gameboy::firmware\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    source = SOURCE.read_text(encoding="utf-8")
    image = assemble(source)
    header = render(source, image)
    if args.check:
        if HEADER.read_text(encoding="utf-8") != header:
            raise SystemExit("SPC700 IPL generated header differs; rebuild it")
    else:
        HEADER.write_text(header, encoding="utf-8", newline="\n")
    if args.output:
        with args.output.open("xb") as output:
            output.write(image)
    print(f"Original SPC700 IPL: {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    main()
