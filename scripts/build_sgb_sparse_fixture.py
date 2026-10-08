#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned solo channels and paired/solo transitions with eight timed onsets."""
import struct
from build_sgb_song_selection_fixture import build_cartridge
from check_sgb_chromatic_reference import PITCHES

CASES=('solo-2','solo-3','transitions')


def bank(articulation=127,case='solo-2'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:
        raise ValueError('invalid sparse fixture case/articulation')
    data=bytearray([0xA5]*2048)
    tables=(0x1FB,0x3FB,0x6AB,0x7AB) if case=='transitions' else (0x1FB,)
    locations=((0x2F1,0x4F5),(0x6FD,0x7EF),(0x6BD,0x6DD),(0x7BD,0x7DD))
    struct.pack_into('<H',data,0,0x2BFF)
    struct.pack_into('<'+'H'*(len(tables)+1),data,0xFF,*(0x2B00+table for table in tables),0)
    for index,table in enumerate(tables):
        data[table:table+16]=bytes(16)
        channels=(2,3) if case=='transitions' and index in (0,3) else (2,) if case=='solo-2' or case=='transitions' and index==1 else (3,)
        for channel in channels:
            offset=locations[index][channel-2]
            struct.pack_into('<H',data,table+2*channel,0x2B00+offset)
            pan=20 if channel==2 else 0
            if case!='transitions' or index in (1,2): pan=10
            prefix=[0xE1,pan,0xED,127]
            if index==0:
                prefix = [0xE0,2,*prefix,0xE7,96]
                if channel==channels[0]: prefix += [0xE5,160]
            if case=='transitions':
                notes=(0x98,0x99,0x9A,0x9B,0x9C,0x9D,0xA3,0xA4)
                stream=[*prefix,16,articulation,*notes[2*index:2*index+2],0]
            else:
                stream=[*prefix,16,articulation,0x98,0xEF,0xFF,0x30,3,0xA4,0]
            data[offset:offset+len(stream)]=bytes(stream)
    data[0x5FF:0x606]=bytes((0xED,127,0x99,0xED,64,0x9A,0))
    return bytes(data)


def expected(case):
    if case not in CASES: raise ValueError('invalid sparse fixture case')
    notes=(24,25,26,27,28,29,35,36) if case=='transitions' else (24,25,26,25,26,25,26,36)
    masks=(12,12,4,4,8,8,12,12) if case=='transitions' else (4 if case=='solo-2' else 8,)*8
    edges=[]
    for index,(note,mask) in enumerate(zip(notes,masks)):
        voices=[]
        for voice in (2,3):
            if not mask&(1<<voice): continue
            pair=[11,0] if voice==2 else [0,11]
            if case!='transitions': pair=[1,1] if index in (2,4,6,7) else [7,7]
            elif mask!=12: pair=[7,7]
            voices.append({'voice':voice,'pitch':PITCHES[note],'volumes':pair,'srcn':2,'adsr1':0x8F,'adsr2':0x6F,'gain':0xB8})
        edges.append({'mask':mask,'voices':voices})
    return edges


def build(articulation=127,case='solo-2'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
