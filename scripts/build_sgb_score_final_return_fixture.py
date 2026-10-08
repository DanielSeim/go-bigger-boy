#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned returning rest followed by a pending final note/rest after inactivity."""
import struct
from build_sgb_score_tail_fixture import bank as prior_bank
from build_sgb_song_selection_fixture import build_cartridge
CASES=('return-note','return-rest')
RESTS=(4,8)
DURATIONS=(8,16)


def bank(articulation=127,duration=8,case='return-note',rest=4):
    if type(articulation) is not int or articulation not in (63,127) or type(duration) is not int or duration not in DURATIONS or case not in CASES or type(rest) is not int or rest not in RESTS:
        raise ValueError('invalid final return fixture')
    data=bytearray(prior_bank(articulation,'clip-3'))
    # Channel 2 is clipped at tick 32; channel 3 alone plays through tick 64.
    data[0x3FB:0x40B]=bytes(16)
    struct.pack_into('<H',data,0x401,0x31FD)
    data[0x6FD:0x702]=bytes((16,articulation,0x9A,0x9B,0))
    # Returning rests expose whether the old held gate expires before the
    # pending event. Channel 3 ends on that event's tick, before a new pattern.
    data[0x6AB:0x6BB]=bytes(16)
    struct.pack_into('<HH',data,0x6AF,0x31DD,0x31BD)
    voice2=bytes((rest,articulation,0xC9,0xED,64,duration,articulation,
                  0xA0 if case=='return-note' else 0xC9,0))
    data[0x6DD:0x6DD+len(voice2)]=voice2
    data[0x6BD:0x6BD+6]=bytes((rest,articulation,0xC9,0xED,64,0))
    struct.pack_into('<H',data,0x105,0)
    return bytes(data)


def build(articulation=127,duration=8,case='return-note',rest=4):
    data=bank(articulation,duration,case,rest)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
