#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned ready boundary note/rest followed immediately by a channel-2 rest."""
import struct
from build_sgb_score_pending_fixture import bank as prior_bank
from build_sgb_song_selection_fixture import build_cartridge
CASES=('boundary-note','boundary-rest')
DURATIONS=(8,16)


def bank(articulation=127,duration=8,rest_duration=8,case='boundary-note',rest_articulation=None):
    if type(articulation) is not int or articulation not in (63,127) or any(type(v) is not int or v not in DURATIONS for v in (duration,rest_duration)) or case not in CASES:raise ValueError('invalid following-rest fixture')
    rest_articulation=articulation if rest_articulation is None else rest_articulation
    if type(rest_articulation) is not int or rest_articulation not in (63,127):raise ValueError('invalid following-rest articulation')
    data=bytearray(prior_bank(articulation,duration,'pending-rest' if case=='boundary-rest' else 'note-return'))
    # Pattern 1 has both voices. Channel 3 supplies a sounding event while
    # channel 2 rests, then both start a note and finish together.
    struct.pack_into('<H',data,0x3FB+4,0x31BD)
    stream=bytes((rest_duration,rest_articulation,0xC9,16,rest_articulation,0x9C,0))
    data[0x6BD:0x6BD+len(stream)]=stream
    stream=bytes((rest_duration,rest_articulation,0x9A,16,rest_articulation,0x9B,0))
    data[0x6FD:0x6FD+len(stream)]=stream
    stream=bytes((16,articulation,0x9D,0x9E,0))
    data[0x6DD:0x6DD+len(stream)]=stream
    return bytes(data)


def build(articulation=127,duration=8,rest_duration=8,case='boundary-note',rest_articulation=None):
    data=bank(articulation,duration,rest_duration,case,rest_articulation)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
