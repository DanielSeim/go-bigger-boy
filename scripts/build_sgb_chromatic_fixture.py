#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned two-channel chromatic octave, base notes 24..36 inclusive."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

NOTES = tuple(range(24, 37))
CASES = {'chromatic': NOTES,
         'calls': (24, 28, 32)*2+(25, 29)+(26, 27, 30, 31, 33, 34, 35, 36)}


def bank(articulation=127, case='chromatic'):
    if type(articulation) is not int or articulation not in (63, 127):
        raise ValueError('fixture articulation must be 63 or 127')
    if case not in CASES:
        raise ValueError('unknown chromatic fixture case')
    data = bytearray(64)
    struct.pack_into('<H', data, 0, 0x2B10)
    struct.pack_into('<HHH', data, 16, 0x2B20, 0x2B30, 0)
    targets = []
    patterns = (NOTES[:7], NOTES[7:]) if case == 'chromatic' else ((), CASES[case][8:])
    for pattern, notes in enumerate(patterns):
        for channel in (2, 3):
            struct.pack_into('<H', data, 32+16*pattern+2*channel, 0x2B00+len(data))
            if pattern == 0:
                data.extend((0xE0, 2, 0xE1, 10, 0xED, 127))
                if channel == 2:
                    data.extend((0xE5, 160, 0xE7, 96))
            if case == 'calls' and pattern == 0:
                data.append(0xEF)
                targets.append(len(data))
                data.extend((0, 0, 2, 0x99, 0x9D, 0))
            else:
                data.extend((16, articulation, *(0x80+note for note in notes), 0))
    if targets:
        for target in targets:
            struct.pack_into('<H', data, target, 0x2B00+len(data))
        data.extend((16, articulation, 0x98, 0x9C, 0xA0, 0))
    return bytes(data)


def build(articulation=127, case='chromatic'):
    data = bank(articulation, case)
    payload = struct.pack('<HH', len(data), 0x2B00)+data+struct.pack('<HH', 0, 0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--articulation', type=int, choices=(63, 127), default=127)
    parser.add_argument('--case', choices=tuple(CASES), default='chromatic')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.articulation, args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Owned chromatic fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
