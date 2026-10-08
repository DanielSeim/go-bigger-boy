#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned held-127 voice returning directly with a note before final readiness."""
import struct
from build_sgb_score_final_return_fixture import bank as prior_bank
from build_sgb_song_selection_fixture import build_cartridge
CASES=('direct-note','direct-rest')
DURATIONS=(8,16)
ARTICULATIONS=(63,127)


def bank(articulation=63,duration=8,case='direct-note'):
    if type(articulation) is not int or articulation not in ARTICULATIONS or type(duration) is not int or duration not in DURATIONS or case not in CASES:
        raise ValueError('invalid direct-return fixture')
    data=bytearray(prior_bank(127,duration,'return-note' if case=='direct-note' else 'return-rest',8))
    data[0x6DD:0x6E6]=bytes((duration,articulation,0xA0,0xED,64,duration,articulation,0xA1 if case=='direct-note' else 0xC9,0))
    data[0x6BD:0x6C3]=bytes((duration,articulation,0xC9,0xED,64,0))
    return bytes(data)


def build(articulation=63,duration=8,case='direct-note'):
    data=bank(articulation,duration,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
