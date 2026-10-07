#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned two-channel songs with unequal first-pattern track durations."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'short-first': (16, 32), 'long-first': (32, 16), 'both-long': (32, 32)}


def score_payload(case='short-first'):
    if case not in CASES:
        raise ValueError('unknown phrase fixture case')
    bank = bytearray(64)
    struct.pack_into('<H', bank, 0, 0x2B10)
    struct.pack_into('<HHH', bank, 16, 0x2B20, 0x2B30, 0)
    for pattern in range(2):
        table = 32 + 16*pattern
        for channel in (2, 3):
            struct.pack_into('<H', bank, table + channel*2, 0x2B00 + len(bank))
            bank.extend((0xE0, 2, 0xE1, 10, 0xED, 127))
            # Global controls are authored once, on the first active channel.
            if pattern == 0 and channel == 2:
                bank.extend((0xE5, 160, 0xE7, 96))
            duration = CASES[case][channel-2] if pattern == 0 else 16
            note = (24 if channel == 2 else 25) if pattern == 0 else 36
            bank.extend((duration, 0x7F, 0x80+note, 0))
    data = struct.pack('<HH', len(bank), 0x2B00) + bank + struct.pack('<HH', 0, 0x0400)
    return data + bytes(4096-len(data))


def build(case='short-first'):
    return build_cartridge(score_payload(case), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), default='short-first')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Original phrase fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
