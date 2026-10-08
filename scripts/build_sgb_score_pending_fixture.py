#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned boundary events followed by inactive channel 2 and later return."""
import struct
from build_sgb_score_order_fixture import bank as prior_bank
from build_sgb_song_selection_fixture import build_cartridge
CASES=('note-return','rest-return','long-rest-return','pending-rest')
DURATIONS=(8,16)


def bank(articulation=127,duration=8,case='note-return'):
    if type(articulation) is not int or articulation not in (63,127) or type(duration) is not int or duration not in DURATIONS or case not in CASES:raise ValueError('invalid pending fixture')
    data=bytearray(prior_bank(articulation,'end-3-rest' if case=='pending-rest' else 'end-3-note'))
    prefix=[0xE0,2,0xE1,10,0xED,127,0xE7,96,0xE5,160]
    stream=bytes([*prefix,16,articulation,0x98,0x99,0xED,64,duration,articulation,0xC9 if case=='pending-rest' else 0xA0,0])
    data[0x2F1:0x2F1+len(stream)]=stream
    # Keep the established multi-page layout. Pattern 1 now has channel 3;
    # pattern 2 returns channel 2. No proprietary data is used.
    for table,channel,offset in ((0x3FB,3,0x6FD),(0x6AB,2,0x6DD)):
        data[table:table+16]=bytes(16)
        struct.pack_into('<H',data,table+2*channel,0x2B00+offset)
    if case in ('rest-return','long-rest-return'):
        rest=16 if case=='long-rest-return' else 8
        stream=bytes((rest,articulation,0xC9,16,articulation,0x9C,0x9D,0))
        data[0x6DD:0x6DD+len(stream)]=stream
    return bytes(data)


def build(articulation=127,duration=8,case='note-return'):
    data=bank(articulation,duration,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
