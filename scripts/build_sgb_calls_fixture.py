#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned two-pattern banks with shared finite-call bodies and caller continuations."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'once': 1, 'twice': 2, 'thrice': 3}


def bank(case='once'):
    if case not in CASES:
        raise ValueError('unknown native call fixture')
    data = bytearray(64)
    struct.pack_into('<H', data, 0, 0x2B10)
    struct.pack_into('<HHH', data, 16, 0x2B20, 0x2B30, 0)
    targets = []
    for pattern in range(2):
        for channel in (2, 3):
            struct.pack_into('<H', data, 32+16*pattern+channel*2, 0x2B00+len(data))
            if pattern == 0:
                data.extend((0xE0, 2, 0xE1, 10, 0xED, 127))
                if channel == 2:
                    data.extend((0xE5, 160, 0xE7, 96))
                data.append(0xEF)
                targets.append(len(data))
                data.extend((0, 0, CASES[case], 0xA4, 0x98, 0))
            else:
                data.extend((16, 127, 0x99, 0xA4, 0))
    for target in targets:
        struct.pack_into('<H', data, target, 0x2B00+len(data))
    data.extend((16, 127, 0x98, 0x99, 0))
    return bytes(data)


def build(case='once'):
    data = bank(case)
    payload = struct.pack('<HH', len(data), 0x2B00)+data+struct.pack('<HH', 0, 0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)), (1,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=tuple(CASES), default='once')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f'Owned native call fixture: {hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
