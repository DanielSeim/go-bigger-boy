#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned trailing ED controls and first-end clipping on channels 2/3."""
import struct
from build_sgb_sparse_fixture import bank as sparse_bank,expected as sparse_expected
from build_sgb_inherit_fixture import LOCATIONS
from build_sgb_song_selection_fixture import build_cartridge
CASES=('tail-2','tail-3','pan-2','pan-3','clip-2','clip-3')
QUALIFIED_CASES=('tail-2','tail-3','pan-2','pan-3')


def bank(articulation=127,case='tail-2'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:raise ValueError('invalid tail fixture')
    data=bytearray(sparse_bank(articulation,'transitions'))
    selected=int(case[-1])
    for index,(offset,channel) in enumerate(LOCATIONS):
        pattern=0 if index<2 else 1 if index==2 else 2 if index==3 else 3
        prefix=[]
        if pattern==0:
            prefix=[0xE0,2,0xE1,10,0xED,127,0xE7,96]
            if channel==2:prefix += [0xE5,160]
        notes=(0x98,0x99,0x9A,0x9B,0x9C,0x9D,0xA3,0xA4)[2*pattern:2*pattern+2]
        stream=[*prefix,16,articulation,notes[0]]
        if pattern==0 and case.startswith('clip') and channel!=selected:stream += [24,articulation]
        stream += [notes[1]]
        if pattern==0 and channel==selected:
            stream += [0xE1,20 if channel==2 else 0] if case.startswith('pan') else [0xED,64]
        if pattern==0 and case.startswith('clip') and channel!=selected:stream += [0xED,64]
        stream += [0]
        data[offset:offset+len(stream)]=bytes(stream)
    return bytes(data)


def expected(case):
    if case not in CASES:raise ValueError('invalid tail fixture')
    edges=sparse_expected('transitions')
    selected=int(case[-1])
    for index,edge in enumerate(edges):
        for voice in edge['voices']:
            pair=[7,7]
            if index>=2 and voice['voice']==selected and (selected==2 or case.startswith('clip')):
                pair=([11,0] if selected==2 else [0,11]) if case.startswith('pan') else [1,1]
            voice['volumes']=pair
    return edges


def build(articulation=127,case='tail-2'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
