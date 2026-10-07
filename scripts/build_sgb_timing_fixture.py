#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned three-note songs for black-box tempo/articulation observations."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'baseline': (96, 0x7F), 'double-tempo': (192, 0x7F),
         'short-gate': (96, 0x3F)}
NOTES = (24, 25, 36)
DURATION = 16


def score_payload(case='baseline'):
    if case not in CASES:
        raise ValueError('unknown timing fixture case')
    tempo, articulation = CASES[case]
    return timing_payload(tempo, articulation, DURATION)


def timing_payload(tempo, articulation, duration):
    if (type(tempo) is not int or not 1 <= tempo <= 255 or
            type(articulation) is not int or not 0 <= articulation <= 127 or
            type(duration) is not int or not 1 <= duration <= 127):
        raise ValueError('invalid owned timing parameters')
    bank = bytearray(36)
    struct.pack_into('<H', bank, 0, 0x2B10)
    struct.pack_into('<HH', bank, 16, 0x2B14, 0)
    struct.pack_into('<H', bank, 24, 0x2B24)  # Pattern channel 2 only.
    bank.extend((0xE0, 2, 0xE1, 10, 0xE5, 160, 0xED, 127, 0xE7, tempo,
                 duration, articulation))
    bank.extend(0x80 + note for note in NOTES)
    bank.extend((1, 0xC9, 0))
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096 - len(data))


def build(case='baseline'):
    return build_cartridge(score_payload(case), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), default='baseline')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original timing fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
