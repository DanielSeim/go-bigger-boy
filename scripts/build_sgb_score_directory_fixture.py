#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned three-song bank and real upload/selection cartridge."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_short_pair_fixture import bank as short_bank
from build_sgb_score_short_continue_fixture import bank as continuation_bank
from build_sgb_song_selection_fixture import build_cartridge

ROOTS = (0x2B20, 0x2B30, 0x2B40)
FAULTS = ('root-zero', 'root-outside', 'root-directory', 'root-last-byte',
          'root-alias', 'root-alias-second', 'bad-stream')


def bank(fault=None):
    data = bytearray(short_bank())
    struct.pack_into('<HHH', data, 0, *ROOTS)
    for offset, table in ((0x20, 0x31AB), (0x30, 0x2B60), (0x40, 0x2B80)):
        struct.pack_into('<HHHH', data, offset, 0x2CFB, 0x2EFB, table, 0)
    rest = short_bank(63, 'continue-rest')
    continuation = continuation_bank(127, 8, 'continue-note')
    for offset, voice2, voice3 in ((0x60, 0x2BC0, 0x31BD), (0x80, 0x2BE0, 0x2BD0)):
        struct.pack_into('<8H', data, offset, 0, 0, voice2, voice3, 0, 0, 0, 0)
    data[0xC0:0xC9] = rest[0x6DD:0x6E6]
    data[0xD0:0xD9] = continuation[0x6BD:0x6C6]
    data[0xE0:0xE9] = continuation[0x6DD:0x6E6]
    if fault is not None:
        if fault not in FAULTS:
            raise ValueError('unknown directory fault')
        if fault == 'bad-stream':
            data[0xE2] = 0xFF  # Unadmitted returning note in song 3.
        else:
            root = {'root-zero': 0, 'root-outside': 0x3300, 'root-directory': 0x2B04,
                    'root-last-byte': 0x32FF, 'root-alias': ROOTS[0], 'root-alias-second': ROOTS[1]}[fault]
            struct.pack_into('<H', data, 4, root)
    return bytes(data)


def build(order=(1,), *, active=False, repeat_upload=False, fault=None):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or not 0 <= song <= 255 for song in order):
        raise ValueError('requires 1..4 bounded song IDs')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    data = bank(fault)
    payload = struct.pack('<HH', len(data), 0x2B00) + data + struct.pack('<HH', 0, 0x0400)
    payload += bytes(4096 - len(payload))
    commands = []
    for index, song in enumerate(order):
        if repeat_upload and index:
            commands.append((8 if active else 64, bytes((0x49,))))
        commands.append((64 if not index or repeat_upload or not active else 8,
                         bytes((0x41, 0, 0, 0, song))))
    return build_cartridge(payload, (1,), commands=commands)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--order', default='1')
    parser.add_argument('--active', action='store_true')
    parser.add_argument('--repeat-upload', action='store_true')
    args = parser.parse_args()
    try:
        image = build(tuple(int(value) for value in args.order.split(',')),
                      active=args.active, repeat_upload=args.repeat_upload)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())


if __name__ == '__main__':
    main()
