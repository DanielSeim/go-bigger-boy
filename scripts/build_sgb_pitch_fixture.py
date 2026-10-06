#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build original single-note songs for black-box pitch/instrument observations."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

NOTES = (24, 25, 36)


def score_payload(instrument=2):
    if type(instrument) is not int or instrument not in (2, 10):
        raise ValueError('fixture instrument must be 2 or 10')
    bank = bytearray(80)
    struct.pack_into('<HHH', bank, 0, 0x2B10, 0x2B14, 0x2B18)
    for index, note in enumerate(NOTES):
        table = 32 + 16 * index
        struct.pack_into('<HH', bank, 16 + 4 * index, 0x2B00 + table, 0)
        # Channel 2 only. Explicit controls bound the note behavior; E0 refers
        # to the caller's resident reference instrument, never a copied sample.
        struct.pack_into('<H', bank, table + 4, 0x2B00 + len(bank))
        bank.extend((0xE0, instrument, 0xE1, 10, 0xE5, 160, 0xED, 127,
                     0xE7, 96, 16, 0x7F, 0x80 + note, 1, 0xC9, 0))
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096 - len(data))


def build(instrument=2):
    return build_cartridge(score_payload(instrument), (1, 2, 3))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--instrument', type=int, choices=(2, 10), default=2)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.instrument)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original pitch fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
