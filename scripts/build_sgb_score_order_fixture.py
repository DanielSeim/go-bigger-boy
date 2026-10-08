#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned simultaneous end/note and end/rest fixtures."""
import struct
from build_sgb_score_tail_fixture import bank as prior_bank,expected as prior_expected
from build_sgb_song_selection_fixture import build_cartridge
CASES=('end-2-note','end-2-rest','end-3-note','end-3-rest')
QUALIFIED_CASES=CASES[:2]


def bank(articulation=127,case='end-2-note'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:raise ValueError('invalid ordering fixture')
    ending=int(case[4]);peer=5-ending
    data=bytearray(prior_bank(articulation,f'tail-{ending}'))
    prefix=[0xE0,2,0xE1,10,0xED,127,0xE7,96]
    if peer==2:prefix += [0xE5,160]
    # These controls and timing changes expose whether the same-tick event
    # executes. Later patterns omit them and inherit only executed values.
    stream=bytes([*prefix,16,articulation,0x98,0x99,0xED,64,8,63,
                  0xC9 if case.endswith('rest') else 0xA0,0])
    offset=0x4F5 if peer==3 else 0x2F1
    data[offset:offset+len(stream)]=stream
    return bytes(data)


def expected(case):
    if case not in CASES:raise ValueError('invalid ordering fixture')
    edges=prior_expected(f'tail-{case[4]}')
    if case.startswith('end-3'):
        for edge in edges[2:]:
            for voice in edge['voices']:voice['volumes']=[1,1]
    return edges


def build(articulation=127,case='end-2-note'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
