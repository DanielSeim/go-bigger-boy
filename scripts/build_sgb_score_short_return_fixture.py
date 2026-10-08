#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned duration-4 direct return with independently known pending durations."""
import struct
from build_sgb_score_direct_return_fixture import bank as prior_bank,CASES,DURATIONS,ARTICULATIONS
from build_sgb_song_selection_fixture import build_cartridge


def bank(articulation=63,duration=8,case='direct-note'):
    data=bytearray(prior_bank(articulation,duration,case))
    data[0x6DD]=4
    data[0x6BD]=4
    return bytes(data)


def build(articulation=63,duration=8,case='direct-note'):
    data=bank(articulation,duration,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
