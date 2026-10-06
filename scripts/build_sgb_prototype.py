#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build original diagnostic SGB LoROM without a proprietary image/toolchain."""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / 'firmware/sgb'
HEADER = FIRMWARE / 'prototype_image.hpp'


def assemble(source, cpu, origin, constants=None):
    """Strict two-pass subset; all instruction widths are explicit/fixed here."""
    implied = ({'sei': 0x78, 'clc': 0x18, 'xce': 0xfb, 'txa': 0x8a,
                'inx': 0xe8, 'dey': 0x88, 'tax': 0xaa, 'tya': 0x98,
                'rts': 0x60, 'iny': 0xc8, 'dex': 0xca}
               if cpu == 'host' else {'clrc': 0x60, 'lsr a': 0x5c, 'xcn a': 0x9f, 'ret': 0x6f})
    branches = ({'bne': 0xd0, 'beq': 0xf0, 'bra': 0x80, 'bcs': 0xb0, 'bcc': 0x90} if cpu == 'host'
                else {'bne': 0xd0, 'beq': 0xf0, 'bmi': 0x30, 'bra': 0x2f})
    labels = dict(constants or {})
    code, fixups = bytearray(), []

    def operand(text, width, relative=False):
        offset = len(code)
        code.extend(bytes(width))
        fixups.append((offset, text, width, relative))

    for number, raw in enumerate(source.splitlines(), 1):
        line = raw.split(';', 1)[0].strip().lower()
        if not line:
            continue
        if line.endswith(':'):
            name = line[:-1]
            if not re.fullmatch(r'[a-z_][a-z_0-9]*', name) or name in labels:
                raise ValueError(f'invalid/duplicate label: {name}')
            labels[name] = origin + len(code)
            continue
        mnemonic, _, args = line.partition(' ')
        args = args.strip()
        if line in implied:
            code.append(implied[line])
        elif mnemonic in branches:
            code.append(branches[mnemonic])
            operand(args, 1, True)
        elif mnemonic == '.org':
            address = int(args.removeprefix('$'), 16)
            if address < origin + len(code):
                raise ValueError('overlapping .org')
            code.extend(bytes(address-origin-len(code)))
        elif mnemonic == '.byte':
            for value in args.split(','):
                operand(value.strip(), 1)
        elif cpu == 'host':
            wide = mnemonic.endswith('.w')
            mnemonic = mnemonic.removesuffix('.w')
            immediate = args.startswith('#')
            indexed = args.endswith(',x')
            forms = {
                ('rep', True, False): (0xc2, 1),
                ('sep', True, False): (0xe2, 1),
                ('ldy', True, False): (0xa0, 2),
                ('adc', True, False): (0x69, 1),
                ('jmp', False, False): (0x4c, 2),
                ('jsr', False, False): (0x20, 2),
                ('ldy', False, True): (0xbc, 2),
                ('sty', False, False): (0x8c, 2),
                ('stx', False, False): (0x8e, 2),
                ('ldx', False, False): (0xae, 2),
                ('adc', False, False): (0x6d, 2),
                ('lda', True, False): (0xa9, 1),
                ('ldx', True, False): (0xa2, 2),
                ('cmp', True, False): (0xc9, 1),
                ('cpx', True, False): (0xe0, 2),
                ('and', True, False): (0x29, 1),
                ('ora', True, False): (0x09, 1),
                ('lda', False, False): (0xad, 2),
                ('lda', False, True): (0xbd, 2),
                ('sta', False, False): (0x8d, 2),
                ('sta', False, True): (0x9d, 2),
                ('cmp', False, False): (0xcd, 2),
                ('ora', False, False): (0x0d, 2),
                ('inc', False, False): (0xee, 2),
                ('stz', False, False): (0x9c, 2),
            }
            key = (mnemonic, immediate, indexed)
            if key not in forms:
                raise ValueError(f'unsupported host instruction {number}: {line}')
            opcode, width = forms[key]
            if wide and immediate:
                if mnemonic not in {'lda', 'cmp', 'adc'}:
                    raise ValueError(f'invalid wide immediate: {line}')
                width = 2
            code.append(opcode)
            operand(args.removeprefix('#').removesuffix(',x'), width)
        elif cpu == 'spc':
            if mnemonic == 'dec':
                code.append(0x8b)
                operand(args, 1)
                continue
            if mnemonic in {'jmp', 'call'}:
                code.append(0x5f if mnemonic == 'jmp' else 0x3f)
                operand(args, 2)
                continue
            parts = [v.strip() for v in args.split(',')]
            if len(parts) != 2:
                raise ValueError(f'unsupported SPC instruction: {line}')
            left, right = parts
            if mnemonic == 'mov' and right.startswith('#') and left.startswith('$'):
                code.append(0x8f)
                operand(right[1:], 1)
                operand(left, 1)
            elif mnemonic == 'mov' and left == 'x' and right == 'a':
                code.append(0x5d)
            elif mnemonic == 'mov' and left == 'a' and right.startswith('#'):
                code.append(0xe8)
                operand(right[1:], 1)
            elif mnemonic == 'mov' and left == 'a':
                address = right.removesuffix('+x')
                absolute = int(address[1:], 16) > 255
                code.append((0xf5 if absolute else 0xf4) if right.endswith('+x') else (0xe5 if absolute else 0xe4))
                operand(address, 2 if absolute else 1)
            elif mnemonic == 'mov' and right == 'a':
                code.append(0xc4)
                operand(left, 1)
            elif mnemonic == 'cmp' and left == 'x' and right.startswith('#'):
                code.append(0xc8)
                operand(right[1:], 1)
            elif mnemonic == 'cmp' and left == 'a':
                code.append(0x68 if right.startswith('#') else 0x64)
                operand(right.removeprefix('#'), 1)
            elif mnemonic == 'adc' and left == 'a' and not right.startswith('#'):
                code.append(0x84)
                operand(right, 1)
            elif mnemonic in {'or', 'and', 'adc'} and left == 'a' and right.startswith('#'):
                code.append({'or': 0x08, 'and': 0x28, 'adc': 0x88}[mnemonic])
                operand(right[1:], 1)
            else:
                raise ValueError(f'unsupported SPC instruction {number}: {line}')
        else:
            raise ValueError(f'unsupported instruction: {line}')
    for offset, value, width, relative in fixups:
        resolved = int(value[1:], 16) if value.startswith('$') else labels[value]
        if relative:
            resolved -= origin + offset + 1
            if not -128 <= resolved <= 127:
                raise ValueError(f'branch out of range: {value}')
            resolved &= 255
        if not 0 <= resolved < 1 << (8*width):
            raise ValueError(f'operand out of range: {value}')
        code[offset:offset+width] = resolved.to_bytes(width, 'little')
    return bytes(code)


def build():
    driver = assemble((FIRMWARE / 'driver.asm').read_text(), 'spc', 0x200)
    samples = assemble((FIRMWARE / 'samples.asm').read_text(), 'spc', 0x500)
    if len(driver) > 0x300:
        raise ValueError('driver overlaps sample directory')
    music = assemble((FIRMWARE / 'music.asm').read_text(), 'spc', 0x700)
    effects = assemble((FIRMWARE / 'effects.asm').read_text(), 'spc', 0x640)
    if len(samples) > 0x140 or len(effects) > 0xc0 or len(music) > 0x100:
        raise ValueError('sample/effects/music sections overlap')
    payload = driver + bytes(0x300-len(driver)) + samples
    payload += bytes(0x440-len(payload)) + effects
    payload += bytes(0x500-len(payload)) + music
    host = assemble((FIRMWARE / 'host.asm').read_text(), 'host', 0x8000,
                    {'payload_size': len(payload), 'entry_token': ((len(payload)+2) | 1) & 255})
    if len(host) > 0x1000 or len(payload) > 0x6fc0:
        raise ValueError('ROM sections overlap')
    rom = bytearray(0x40000)
    rom[:len(host)] = host
    rom[0x1000:0x1000+len(payload)] = payload
    rom[0x7fc0:0x7fd5] = b'GBB ORIGINAL SGB TEST'
    rom[0x7fd5:0x7fda] = bytes((0x20, 0, 8, 0, 1))
    rom[0x7ffc:0x8000] = bytes((0, 0x80, 0, 0x80))
    checksum = (sum(rom) + 510) & 65535
    rom[0x7fdc:0x7fe0] = (checksum ^ 65535).to_bytes(2, 'little') + checksum.to_bytes(2, 'little')
    return bytes(rom), host, payload


def render(rom, host, payload):
    def array(name, data):
        rows = ['    ' + ', '.join(f'0x{x:02X}' for x in data[i:i+16]) + ','
                for i in range(0, len(data), 16)]
        return (f'inline constexpr std::array<std::uint8_t, {len(data)}> {name}{{\n'
                + '\n'.join(rows) + '\n};\n')
    return ('// SPDX-License-Identifier: GPL-3.0-or-later\n'
            '// Generated by scripts/build_sgb_prototype.py; diagnostic use only.\n'
            f'// LoROM SHA-256: {hashlib.sha256(rom).hexdigest()}\n'
            '#pragma once\n#include <algorithm>\n#include <array>\n#include <cstdint>\n#include <vector>\n'
            'namespace gameboy::firmware {\n' + array('sgb_prototype_host', host)
            + array('sgb_prototype_spc', payload) + array('sgb_prototype_header', rom[0x7fc0:0x8000])
            + 'inline std::vector<std::uint8_t> sgb_prototype_rom() {\n'
            '    std::vector<std::uint8_t> rom(0x40000);\n'
            '    std::copy(sgb_prototype_host.begin(), sgb_prototype_host.end(), rom.begin());\n'
            '    std::copy(sgb_prototype_spc.begin(), sgb_prototype_spc.end(), rom.begin()+0x1000);\n'
            '    std::copy(sgb_prototype_header.begin(), sgb_prototype_header.end(), rom.begin()+0x7fc0);\n'
            '    return rom;\n}\n} // namespace gameboy::firmware\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--output', type=Path, help='Create a diagnostic LoROM; refuses to overwrite')
    args = parser.parse_args()
    rom, host, payload = build()
    header = render(rom, host, payload)
    if args.check:
        if HEADER.read_text() != header:
            raise SystemExit('generated prototype header differs; rebuild')
    else:
        HEADER.write_text(header, encoding='utf-8', newline='\n')
    if args.output:
        with args.output.open('xb') as output:
            output.write(rom)
    print(f'Original SGB prototype: {hashlib.sha256(rom).hexdigest()}')


if __name__ == '__main__':
    main()
