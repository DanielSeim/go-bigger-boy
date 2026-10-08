#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned final channel-3 end with a ready channel-2 boundary note/rest."""
import struct
from build_sgb_score_pending_fixture import bank as prior_bank
from build_sgb_song_selection_fixture import build_cartridge
CASES=('final-note','final-rest')
DURATIONS=(8,16)


def bank(articulation=127,duration=8,case='final-note'):
    if type(articulation) is not int or articulation not in (63,127) or type(duration) is not int or duration not in DURATIONS or case not in CASES:raise ValueError('invalid final boundary fixture')
    data=bytearray(prior_bank(articulation,duration,'pending-rest' if case=='final-rest' else 'note-return'))
    # Terminate the phrase list immediately after the first owned pattern.
    struct.pack_into('<H',data,0x101,0)
    return bytes(data)


def build(articulation=127,duration=8,case='final-note'):
    data=bank(articulation,duration,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
