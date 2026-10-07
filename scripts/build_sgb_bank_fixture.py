#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned page-crossing phrase tables, streams, finite calls and returns."""
import struct
from build_sgb_mix_fixture import CASES, bank as compact_bank
from build_sgb_song_selection_fixture import build_cartridge


def bank(articulation=127, case='pan-calls', size=2048):
    if type(size) is not int or size not in (1619, 2048):
        raise ValueError('owned bank size must be 1619 or 2048')
    compact = compact_bank(articulation, case)
    data = bytearray([0xA5]*size)
    phrase, tables = 0xFF, (0x1FB, 0x3FB)
    tracks = (0x2F1, 0x4F5, 0x6FD if size == 2048 else 0x5ED, size-9)
    body = 0x5FF
    struct.pack_into('<H', data, 0, 0x2B00+phrase)
    struct.pack_into('<HHH', data, phrase, *(0x2B00+x for x in tables), 0)
    for pattern, table in enumerate(tables):
        data[table:table+16] = bytes(16)
        for channel in (2, 3):
            index = pattern*2+channel-2
            offset = tracks[index]
            struct.pack_into('<H', data, table+2*channel, 0x2B00+offset)
            old = int.from_bytes(compact[32+16*pattern+2*channel:34+16*pattern+2*channel], 'little')-0x2B00
            length = (19 if channel == 2 else 15) if pattern == 0 else 9
            stream = bytearray(compact[old:old+length])
            if pattern == 0:
                call = stream.index(0xEF)
                struct.pack_into('<H', stream, call+1, 0x2B00+body)
            data[offset:offset+length] = stream
    data[body:body+9] = compact[-9:]
    return bytes(data)


def build(articulation=127, case='pan-calls'):
    data = bank(articulation, case)
    payload = struct.pack('<HH', len(data), 0x2B00)+data+struct.pack('<HH', 0, 0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)), (1,))
