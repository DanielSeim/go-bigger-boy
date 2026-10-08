#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned omitted duration/articulation across paired and solo patterns."""
import struct
from build_sgb_sparse_fixture import bank as sparse_bank,expected as sparse_expected
from build_sgb_inherit_fixture import LOCATIONS
from build_sgb_song_selection_fixture import build_cartridge
CASES=('timing-hold','art-hold')


def bank(articulation=127,case='timing-hold'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:
        raise ValueError('invalid timing fixture')
    data=bytearray(sparse_bank(articulation,'transitions'))
    for index,(offset,channel) in enumerate(LOCATIONS):
        pattern=0 if index<2 else 1 if index==2 else 2 if index==3 else 3
        art=articulation if channel==2 else 190-articulation
        prefix=[]
        if pattern==0:
            prefix=[0xE0,2,0xE1,10,0xED,127,0xE7,96]
            if channel==2:prefix += [0xE5,160]
        timing=[16,art] if pattern==0 else [] if case=='timing-hold' else [16]
        if pattern==3 and channel==2:timing=[16,190-articulation]
        notes=(0x98,0x99,0x9A,0x9B,0x9C,0x9D,0xA3,0xA4)[2*pattern:2*pattern+2]
        stream=bytes((*prefix,*timing,*notes,0))
        data[offset:offset+len(stream)]=stream
    return bytes(data)


def expected(case):
    if case not in CASES:raise ValueError('invalid timing fixture')
    edges=sparse_expected('transitions')
    for edge in edges:
        for voice in edge['voices']:voice['volumes']=[7,7]
    return edges


def build(articulation=127,case='timing-hold'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
