#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned short return followed by an ordinary continuing note or rest."""
import struct
from build_sgb_score_short_return_fixture import bank as prior_bank,ARTICULATIONS,DURATIONS
from build_sgb_song_selection_fixture import build_cartridge
CASES=('continue-note','continue-rest')


def bank(articulation=63,duration=8,case='continue-note'):
    if case not in CASES:raise ValueError('invalid short continuation case')
    data=bytearray(prior_bank(articulation,duration,'direct-note' if case=='continue-note' else 'direct-rest'))
    # Channel 3 now continues alongside channel 2, rather than ending at tick 68.
    data[0x6C2:0x6C6]=bytes((duration,articulation,0xC9,0))
    return bytes(data)


def build(articulation=63,duration=8,case='continue-note'):
    data=bank(articulation,duration,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
