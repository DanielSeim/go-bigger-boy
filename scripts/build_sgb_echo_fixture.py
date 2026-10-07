#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned note songs isolating echo routing and setup operands."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

# (send mask, left volume, right volume, delay, feedback, filter).
CASES = {'routing': ((0, 16, 24, 1, 32, 0), (4, 16, 24, 1, 32, 0), (0, 16, 24, 1, 32, 0)),
         'setup': ((4, 16, 24, 1, 32, 0), (4, 16, 24, 2, 32, 0), (4, 16, 24, 1, 64, 0))}


def score_payload(case='routing'):
    if case not in CASES:
        raise ValueError('unknown echo fixture case')
    bank = bytearray(36)
    struct.pack_into('<H', bank, 0, 0x2B10)
    struct.pack_into('<HH', bank, 16, 0x2B14, 0)
    struct.pack_into('<H', bank, 24, 0x2B24)  # Pattern channel 2 only.
    bank.extend((0xE0, 2, 0xE1, 10, 0xE5, 160, 0xED, 127, 0xE7, 96))
    for mask, left, right, delay, feedback, fir in CASES[case]:
        # Disable echo and wait before changing its buffer, then allow setup to
        # settle before enabling a send. One SOUND request prevents interruption.
        bank.extend((0xF6, 64, 0x7F, 0xC9, 0xF7, delay, feedback, fir,
                     16, 0xC9, 0xF5, mask, left, right,
                     16, 0x7F, 0x98, 16, 0xC9))
    bank.append(0)
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096-len(data))


def build(case='routing'):
    return build_cartridge(score_payload(case), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), default='routing')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original echo fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
