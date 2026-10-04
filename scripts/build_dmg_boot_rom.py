#!/usr/bin/env python3
"""Rebuild/check the original DMG firmware; never reads reference ROMs."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware/gameboy/dmg.asm"
HEADER = ROOT / "firmware/gameboy/dmg_boot_image.hpp"


def render(image):
    digest = hashlib.sha256(image).hexdigest()
    # Normalize checkout line endings so Windows and Linux verify identically.
    source_digest = hashlib.sha256(SOURCE.read_text(encoding="utf-8").encode("utf-8")).hexdigest()
    rows = ["    " + ", ".join(f"0x{byte:02X}" for byte in image[i:i + 16]) + ","
            for i in range(0, len(image), 16)]
    return ("// SPDX-License-Identifier: GPL-3.0-or-later\n"
            "// Generated from dmg.asm by scripts/build_dmg_boot_rom.py.\n"
            f"// SHA-256: {digest}\n"
            f"// Source SHA-256 (LF): {source_digest}\n"
            "#pragma once\n\n"
            "#include <array>\n#include <cstdint>\n\n"
            "namespace gameboy::firmware {\n"
            "inline constexpr std::array<std::uint8_t, 256> dmg_boot_image{\n"
            + "\n".join(rows) + "\n};\n} // namespace gameboy::firmware\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    checks = parser.add_mutually_exclusive_group()
    checks.add_argument("--check", action="store_true",
                        help="verify the checked-in header without modifying it")
    checks.add_argument("--check-source", action="store_true",
                        help="verify source/image provenance without requiring RGBDS")
    parser.add_argument("--rgbasm", default="rgbasm")
    parser.add_argument("--rgblink", default="rgblink")
    parser.add_argument("--output", type=Path, help="write an original 256-byte boot image")
    args = parser.parse_args()
    if args.check_source:
        header = HEADER.read_text(encoding="utf-8")
        image = bytes(int(byte, 16) for byte in re.findall(r"0x([0-9A-F]{2})", header))
    else:
        with tempfile.TemporaryDirectory(prefix="gbb-dmg-firmware-") as directory:
            obj = Path(directory) / "dmg.o"
            binary = Path(directory) / "dmg.bin"
            subprocess.run([args.rgbasm, "-Wall", "-Wextra", "-o", str(obj), str(SOURCE)],
                           check=True)
            subprocess.run([args.rgblink, "-x", "-o", str(binary), str(obj)], check=True)
            image = binary.read_bytes()
    if len(image) != 256 or image[-2:] != b"\xE0\x50":
        raise SystemExit("invalid firmware size or unmap instruction")
    generated = render(image)
    if args.check or args.check_source:
        if not HEADER.exists() or HEADER.read_text(encoding="utf-8") != generated:
            raise SystemExit("DMG firmware header differs; regenerate it")
    else:
        HEADER.write_text(generated, encoding="utf-8", newline="\n")
    if args.output:
        if args.output.exists():
            raise SystemExit("refusing to overwrite an existing output image")
        args.output.write_bytes(image)
    print(f"Original DMG firmware: {len(image)} bytes, SHA-256 {hashlib.sha256(image).hexdigest()}")


if __name__ == "__main__":
    main()
