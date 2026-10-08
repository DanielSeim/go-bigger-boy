#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned sparse transitions with one/three instrument-2 selections per prefix."""
import struct
from build_sgb_sparse_fixture import bank as sparse_bank, expected as sparse_expected
from build_sgb_song_selection_fixture import build_cartridge

CASES={'reselect-1':1,'reselect-2':2,'reselect-3':3}


def bank(articulation=127,case='reselect-1'):
    if case not in CASES: raise ValueError('invalid reselection fixture')
    data=bytearray(sparse_bank(articulation,'transitions'))
    locations=((0x6FD,2,(0x9A,0x9B)),(0x6DD,3,(0x9C,0x9D)),
               (0x7BD,2,(0xA3,0xA4)),(0x7DD,3,(0xA3,0xA4)))
    for offset,channel,notes in locations:
        pan=10 if offset<0x700 else 20 if channel==2 else 0
        stream=bytes((*([0xE0,2]*CASES[case]),0xE1,pan,0xED,127,16,articulation,*notes,0))
        data[offset:offset+len(stream)]=stream
    return bytes(data)


def expected(case):
    if case not in CASES: raise ValueError('invalid reselection fixture')
    return sparse_expected('transitions')


def build(articulation=127,case='reselect-1'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
