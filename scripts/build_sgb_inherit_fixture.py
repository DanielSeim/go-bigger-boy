#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned cross-pattern pan/track-volume and inactive-channel carry fixtures."""
import struct
from build_sgb_sparse_fixture import bank as sparse_bank, expected as sparse_expected
from build_sgb_song_selection_fixture import build_cartridge

CASES=('pan-hold','track-hold')
LOCATIONS=((0x2F1,2),(0x4F5,3),(0x6FD,2),(0x6DD,3),(0x7BD,2),(0x7DD,3))


def bank(articulation=127,case='pan-hold'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:
        raise ValueError('invalid inheritance fixture')
    data=bytearray(sparse_bank(articulation,'transitions'))
    for index,(offset,channel) in enumerate(LOCATIONS):
        pattern=0 if index<2 else 1 if index==2 else 2 if index==3 else 3
        prefix=[]
        if pattern==0:
            prefix=[0xE0,2,0xE1,20 if channel==2 else 0,0xED,127] if case=='pan-hold' else [0xE0,2,0xE1,10,0xED,64]
            prefix += [0xE7,96]
            if channel==2: prefix += [0xE5,160]
        elif pattern==3 and channel==2:
            prefix=[0xE1,10] if case=='pan-hold' else [0xED,127]
        notes=(0x98,0x99,0x9A,0x9B,0x9C,0x9D,0xA3,0xA4)[2*pattern:2*pattern+2]
        stream=bytes((*prefix,16,articulation,*notes,0))
        data[offset:offset+len(stream)]=stream
    return bytes(data)


def expected(case):
    if case not in CASES: raise ValueError('invalid inheritance fixture')
    edges=sparse_expected('transitions')
    for index,edge in enumerate(edges):
        for voice in edge['voices']:
            voice['volumes']=([7,7] if index>=6 and voice['voice']==2 else [11,0] if voice['voice']==2 else [0,11]) if case=='pan-hold' else ([7,7] if index>=6 and voice['voice']==2 else [1,1])
    return edges


def build(articulation=127,case='pan-hold'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
