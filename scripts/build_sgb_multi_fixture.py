#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned two-event tracks with inherited duration and bounded phrase clipping."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'short-first': (8, 16), 'long-first': (16, 8), 'both-long': (16, 16)}


def bank(case='short-first'):
    if case not in CASES:
        raise ValueError('unknown multi-event fixture')
    result = bytearray(64)
    struct.pack_into('<H', result, 0, 0x2B10)
    struct.pack_into('<HHH', result, 16, 0x2B20, 0x2B30, 0)
    for pattern in range(2):
        for channel in (2, 3):
            struct.pack_into('<H', result, 32+16*pattern+channel*2, 0x2B00+len(result))
            result.extend((0xE0, 2, 0xE1, 10, 0xED, 127))
            if pattern == 0:
                if channel == 2:
                    result.extend((0xE5, 160, 0xE7, 96))
                result.extend((8, 127, 0x98 if channel == 2 else 0x99))
                duration = CASES[case][channel-2]
                if duration != 8:
                    result.append(duration)
                result.extend((0x99 if channel == 2 else 0x98, 0))
            else:
                result.extend((16, 127, 0xA4, 0))
    return bytes(result)


def build(case='short-first'):
    data = bank(case)
    payload = struct.pack('<HH', len(data), 0x2B00) + data + struct.pack('<HH', 0, 0x0400)
    return build_cartridge(payload + bytes(4096-len(payload)), (1,))


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
    print(f'Owned multi-event fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
