#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned held articulation-127 voice returning through articulation-63 rests."""
import struct
from build_sgb_score_final_return_fixture import bank as prior_bank,CASES,RESTS,DURATIONS
from build_sgb_song_selection_fixture import build_cartridge


def bank(articulation=127,duration=8,case='return-note',rest=4):
    if type(articulation) is not int or articulation!=127:raise ValueError('mixed fixture requires an initially held articulation-127 voice')
    data=bytearray(prior_bank(articulation,duration,case,rest))
    data[0x6DE]=63
    data[0x6E3]=63
    data[0x6BE]=63
    return bytes(data)


def build(articulation=127,duration=8,case='return-note',rest=4):
    data=bank(articulation,duration,case,rest)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
