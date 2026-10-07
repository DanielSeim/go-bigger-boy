#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned mix changes through repeated subroutine calls and returns."""
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = ('pan-calls', 'song-calls')
NOTES = (24, 25, 26, 25, 26, 36, 29, 32)


def volumes(case):
    if case not in CASES:
        raise ValueError('unknown mix case')
    return ([[[11, 0], [0, 11]], [[7, 7]]*2, [[1, 1]]*2, [[7, 7]]*2,
             [[1, 1]]*2, [[1, 1]]*2, [[0, 11], [11, 0]], [[0, 11], [11, 0]]]
            if case == 'pan-calls' else [[[1, 1]]*2 for _ in NOTES])


def bank(articulation=127, case='pan-calls'):
    if type(articulation) is not int or articulation not in (63, 127) or case not in CASES:
        raise ValueError('unknown mix case/articulation')
    data = bytearray(64)
    struct.pack_into('<H', data, 0, 0x2B10)
    struct.pack_into('<HHH', data, 16, 0x2B20, 0x2B30, 0)
    targets = []
    for pattern in (0, 1):
        for channel in (2, 3):
            struct.pack_into('<H', data, 32+16*pattern+2*channel, 0x2B00+len(data))
            pan = (20 if channel == 2 else 0) if pattern == 0 else (0 if channel == 2 else 20)
            if case == 'song-calls':
                pan = 10
            if pattern == 0:
                data.extend((0xE0, 2, 0xE1, pan, 0xED, 127))
                if channel == 2:
                    data.extend((0xE5, 160 if case == 'pan-calls' else 80, 0xE7, 96))
                data.extend((16, articulation, 0x98, 0xEF))
                targets.append(len(data))
                data.extend((0, 0, 2, 0xA4, 0))
            else:
                data.extend((0xE1, pan, 0xED, 127, 16, articulation, 0x9D, 0xA0, 0))
    for target in targets:
        struct.pack_into('<H', data, target, 0x2B00+len(data))
    data.extend((0xE1, 10, 0xED, 127, 0x99, 0xED, 64 if case == 'pan-calls' else 127, 0x9A, 0))
    if len(data) > 128:
        raise ValueError('mix fixture exceeds native bank bound')
    return bytes(data)


def build(articulation=127, case='pan-calls'):
    data = bank(articulation, case)
    payload = struct.pack('<HH', len(data), 0x2B00)+data+struct.pack('<HH', 0, 0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)), (1,))
