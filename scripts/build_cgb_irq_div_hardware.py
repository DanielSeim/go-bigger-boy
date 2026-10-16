#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build authored CGB first-vector DIV witnesses with visible raw results."""
import argparse
import hashlib
import json
from pathlib import Path

import cgb_irq_div_probe as probe
from inspect_cartridge_headers import LOGO

# Independently drawn 3x5 glyphs, expanded into 8x8 Game Boy tiles.
GLYPHS = dict(zip('0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+- ', (
    '111101101101111', '010110010010111', '111001111100111', '111001111001111',
    '101101111001001', '111100111001111', '111100111101111', '111001010010010',
    '111101111101111', '111101111001111', '010101111101101', '110101110101110',
    '111100100100111', '110101101101110', '111100110100111', '111100110100100',
    '111100101101111', '101101111101101', '111010010010111', '001001001101111',
    '101101110101101', '100100100100111', '101111111101101', '101111111111101',
    '111101101101111', '111101111100100', '111101101111001', '110101110101101',
    '111100111001111', '111010010010010', '101101101101111', '101101101101010',
    '101101111111101', '101101010101101', '101101010010010', '111001010100111',
    '000010111010000', '000000111000000', '000000000000000')))
TILES = {char: i + 1 for i, char in enumerate(GLYPHS)}


def tiles():
    data = bytearray(16)  # Tile zero is blank.
    for glyph in GLYPHS.values():
        rows = [0] + [int(glyph[i:i + 3], 2) << 3 for i in range(0, 15, 3)] + [0, 0]
        for row in rows:
            data.extend((row, 0))
    return bytes(data)


def text_code(row, text):
    code = bytearray((0x21, 0, 0x98 + row // 8))
    address = 0x9800 + row * 32
    code[1:3] = address.to_bytes(2, 'little')
    for char in text:
        code.extend((0x3e, TILES[char], 0x22))
    return code


def safe_lcd_off():
    # Skip LY polling when LCD is already off. Otherwise wait for VBlank.
    return bytes.fromhex('f0 40 cb 7f 28 06 f0 44 fe 90 38 fa af e0 40')


def hardware_rom(scenario, double_speed=False, *, missing_irq=False):
    source, phase, read = probe.split(scenario)
    rom = bytearray(probe.probe_rom(scenario, double_speed))
    # Real CGB boot requires the compatibility logo and header checksum.
    rom[0x104:0x134] = LOGO
    rom[0x134:0x143] = b'GBB IRQ DIV'.ljust(15, b'\x00')
    rom[0x100:0x103] = bytes.fromhex('c3 00 08')
    # Safely stop the boot LCD, clear BG attributes in VRAM bank one, then
    # enter the original startup. Everything here precedes the DIV anchor.
    entry = bytes.fromhex('f3') + safe_lcd_off()
    entry += bytes.fromhex('3e 01 e0 4f 21 00 80 01 00 20 af 22 0b 78 b1 20 f9 af e0 4f c3 50 01')
    rom[0x800:0x800 + len(entry)] = entry
    # Replace only the post-request infinite wait with a failure continuation.
    # Its first 16 NOPs are unchanged; valid requests enter the vector earlier.
    tail = bytes(16) + bytes.fromhex('18 fe')
    end = rom.find(tail, 0x150, 0x300)
    if end < 0:
        raise ValueError('IRQ wait layout changed')
    rom[end + 16:end + 19] = bytes.fromhex('c3 00 09')
    if missing_irq:
        # Test-only fault injection: no enabled interrupt can enter the vector.
        ie = rom.index(bytes.fromhex('3e 04 ea ff ff'), 0x150, end)
        rom[ie + 1] = 0
    finish = rom.index(bytes.fromhex('f3 76 18 fd'), 0x300, 0x380)
    rom[finish:finish + 4] = bytes.fromhex('f3 c3 20 09')
    failure = bytes.fromhex('f3 af e0 07 ea ff ff 3e 01 ea 03 c0 ea 00 c0 ea 01 c0 c3 20 09')
    rom[0x900:0x900 + len(failure)] = failure
    # Validate captured structure, never the disputed DIV=0/1 choice.
    checks = bytearray.fromhex('fa 03 c0 b7 c2 00 0a fa 00 c2 fe a5 c2 00 09')
    checks += bytes.fromhex('fa 02 c1 e6 1f c2 00 09 fa 00 c1 fe 02 d2 00 09')
    if source != 'timer':
        checks += bytes.fromhex('fa 01 c1 fe ff c2 00 09')
    checks += bytes.fromhex('c3 00 0a')
    rom[0x920:0x920 + len(checks)] = checks
    # Draw after the frozen capture. No display work enters the measured path.
    draw = bytearray(safe_lcd_off())
    font = tiles()
    draw += bytes.fromhex('21 00 10 11 00 80') + bytes((0x01, len(font) & 255, len(font) >> 8))
    draw += bytes.fromhex('2a 12 13 0b 78 b1 20 f8')
    draw += bytes.fromhex('af e0 4f 3e 80 e0 68 21 00 13 06 08 2a e0 69 05 20 fa')
    draw += text_code(0, 'CGB IRQ DIV')
    draw += text_code(2, 'TIMER' if source == 'timer' else 'SW ' + {-4: '-4', 0: '0', 4: '+4'}[probe.SOFTWARE_OFFSETS[source]])
    draw += text_code(3, 'SPEED ' + ('2' if double_speed else '1'))
    draw += text_code(4, f'START {phase:02X} READ {read:X}')
    for row, label, address in ((6, 'DIV ', 0xc100), (7, 'TIMA ', 0xc101),
                                (8, 'IF ', 0xc102), (9, 'GUARD ', 0xc200)):
        draw += text_code(row, label)
        draw += bytes((0xfa, address & 255, address >> 8)) + bytes.fromhex('cd 00 0c')
    draw += text_code(11, 'DONE')
    # Replace DONE with ERROR on failure, without hiding the raw values.
    draw += bytes.fromhex('fa 03 c0 b7 28 12') + text_code(11, 'ERROR')
    draw += bytes.fromhex('af e0 42 e0 43 3e fc e0 47 3e 91 e0 40')
    # Mooneye PASS means structural completion, not a hardware timing verdict.
    draw += bytes.fromhex('fa 03 c0 b7 c2 20 0c 01 05 03 11 0d 08 21 22 15 40 76 18 fd')
    if 0xa00 + len(draw) > 0xc00:
        raise ValueError('display routine overlaps hex writer')
    rom[0xa00:0xa00 + len(draw)] = draw
    # Write A in hex to the tile cursor HL, preserving the captured WRAM.
    hex_code = bytes.fromhex('f5 cb 37 e6 0f c6 01 22 f1 e6 0f c6 01 22 c9')
    rom[0xc00:0xc00 + len(hex_code)] = hex_code
    rom[0xc20:0xc20 + 13] = bytes.fromhex('01 42 42 11 42 42 21 42 42 40 76 18 fd')
    rom[0x1000:0x1000 + len(font)] = font
    rom[0x1300:0x1308] = bytes.fromhex('ff 7f 00 00 ff 7f ff 7f')
    rom[0x14d:0x150] = bytes(3)
    checksum = 0
    for value in rom[0x134:0x14d]:
        checksum = (checksum - value - 1) & 255
    rom[0x14d] = checksum
    rom[0x14e:0x150] = (sum(rom) & 65535).to_bytes(2, 'big')
    return bytes(rom)


def build(output, phase=44, read=3):
    output.mkdir(parents=True, exist_ok=False)
    rows = []
    for speed in (False, True):
        for source in ('timer', *probe.SOFTWARE_OFFSETS):
            scenario = f'{source}-start-{phase}-read-{read}'
            data = hardware_rom(scenario, speed)
            name = f'{scenario}-{"double" if speed else "normal"}.gbc'
            (output / name).write_bytes(data)
            rows.append(dict(file=name, scenario=scenario, double_speed=speed,
                             sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(schema='gbb-cgb-irq-div-hardware-v1', qualification=False,
                    source_sha256={path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in (Path(__file__), Path(probe.__file__),
                                                Path(probe.boundary.__file__), Path(probe.isolated.__file__))},
                    protocol='Mooneye signature checks completion only; read raw DIV on screen', rows=rows)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'README.txt').write_text(
        'CGB first-vector DIV witness (authored diagnostic ROMs)\n\n'
        'Use a CGB-compatible flash cartridge. Normal/double means CPU speed.\n'
        'timer uses hardware overflow; software-early/software/software-late\n'
        'assert timer IF at predicted reload minus four/zero/plus four T-cycles.\n'
        'DONE and Mooneye PASS mean structural completion, not timing parity.\n'
        'DIV 00 and 01 are both accepted observations. ERROR indicates invalid\n'
        'completion, guard, IF acknowledgement, or stopped software TIMA.\n'
        'Register and START/READ fields are hexadecimal; IF E0 means low flags\n'
        'clear and GUARD should be A5. The raw screen persists until reset.\n\n'
        'Record ROM SHA-256 from manifest.json, console model/revision, flash\n'
        'cartridge/loader version, and photograph each result. Repeat on reset.\n'
        'No physical-hardware timing result is assumed or bundled.\n')

    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--start-phase', type=int, choices=(43, 44, 47, 48, 51, 52), default=44)
    parser.add_argument('--read-phase', type=int, choices=range(4), default=3)
    args = parser.parse_args()
    try:
        print(json.dumps(build(args.output, args.start_phase, args.read_phase)))
        return 0
    except (OSError, ValueError) as error:
        parser.exit(2, f'hardware DIV build failed: {error}\n')


if __name__ == '__main__':
    raise SystemExit(main())
