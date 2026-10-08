#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned duration-four return followed by another duration-four note or rest."""
import struct
from build_sgb_score_short_continue_fixture import bank as prior_bank,CASES,ARTICULATIONS
from build_sgb_song_selection_fixture import build_cartridge


def bank(articulation=63,case='continue-note'):
    data=bytearray(prior_bank(articulation,8,case))
    data[0x6E2]=4
    data[0x6C2]=4
    return bytes(data)


def build(articulation=63,case='continue-note'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
