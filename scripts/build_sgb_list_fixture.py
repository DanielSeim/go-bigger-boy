#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned three/four-pattern calls, controls and page-crossing phrase tables."""
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'three-patterns': 'pan-calls', 'four-patterns': 'song-calls'}


def bank(articulation=127, case='three-patterns'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:
        raise ValueError('unknown phrase-list case/articulation')
    count = 3 if case == 'three-patterns' else 4
    pan_case = case == 'three-patterns'
    data = bytearray([0xA5]*2048)
    tables = (0x1FB,0x3FB,0x6AB,0x7AB)[:count]
    locations = ((0x2F1,0x4F5),(0x6FD,0x7EF),(0x6BD,0x6DD),(0x7BD,0x7DD))
    struct.pack_into('<H',data,0,0x2BFF)
    struct.pack_into('<'+'H'*(count+1),data,0xFF,*(0x2B00+t for t in tables),0)
    for pattern,table in enumerate(tables):
        data[table:table+16] = bytes(16)
        for channel,offset in zip((2,3),locations[pattern]):
            struct.pack_into('<H',data,table+2*channel,0x2B00+offset)
            pan = (20 if channel==2 else 0) if pattern==0 else (0 if channel==2 else 20)
            if not pan_case: pan = 10
            prefix = [0xE1,pan,0xED,127]
            if pattern==0:
                prefix = [0xE0,2,*prefix]
                if channel==2: prefix += [0xE5,160 if pan_case else 80,0xE7,96]
            if pattern==0 and pan_case:
                stream = [*prefix,16,articulation,0x98,0xEF,0xFF,0x30,2,0xA4,0]
            elif pattern==1 and not pan_case:
                stream = [16,articulation,0xEF,0xFF,0x30,2,0xA4,0]
            else:
                note = 0x98 if pattern==0 else 0x9D if (pan_case and pattern==1 or not pan_case and pattern==2) else 0xA0
                stream = [*prefix,16,articulation,note,0]
            data[offset:offset+len(stream)] = bytes(stream)
    data[0x5FF:0x608] = bytes((0xE1,10,0xED,127,0x99,0xED,64 if pan_case else 127,0x9A,0))
    return bytes(data)


def build(articulation=127, case='three-patterns'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
