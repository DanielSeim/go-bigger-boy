#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned single-note songs isolating pan, song volume and track volume."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

# (pan, song volume, track volume). Every song explicitly restores controls.
CASES = {'pan': ((10, 160, 127), (0, 160, 127), (20, 160, 127)),
         'volume': ((10, 160, 127), (10, 80, 127), (10, 160, 64))}


def score_payload(case='pan'):
    if case not in CASES:
        raise ValueError('unknown volume/pan fixture case')
    bank = bytearray(80)
    struct.pack_into('<HHH', bank, 0, 0x2B10, 0x2B14, 0x2B18)
    for index, (pan, song_volume, track_volume) in enumerate(CASES[case]):
        table = 32 + 16 * index
        struct.pack_into('<HH', bank, 16 + 4 * index, 0x2B00 + table, 0)
        struct.pack_into('<H', bank, table + 4, 0x2B00 + len(bank))
        bank.extend((0xE0, 2, 0xE1, pan, 0xE5, song_volume, 0xED, track_volume,
                     0xE7, 96, 16, 0x7F, 0x98, 1, 0xC9, 0))
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096-len(data))


def build(case='pan'):
    return build_cartridge(score_payload(case), (1, 2, 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), default='pan')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original volume/pan fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
