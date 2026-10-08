#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned clipped-peer notes, inactive freeze and rest reactivation."""
from build_sgb_score_tail_fixture import bank as tail_bank,build as tail_build,expected as tail_expected
from build_sgb_song_selection_fixture import build_cartridge
import struct
CASES=('clip-2','clip-3','rest-2','rest-3')


def bank(articulation=127,case='clip-2'):
    if case not in CASES:raise ValueError('invalid peer fixture')
    data=bytearray(tail_bank(articulation,'clip-'+case[-1]))
    if case.startswith('rest'):
        # Reactivate the clipped voice with a rest before its first note.
        offset=0x6DD if case[-1]=='2' else 0x6FD
        notes=(0x9C,0x9D) if case[-1]=='2' else (0x9A,0x9B)
        stream=bytes((8,articulation,0xC9,16,articulation,*notes,0))
        data[offset:offset+len(stream)]=stream
    return bytes(data)


def expected(case):
    if case not in CASES:raise ValueError('invalid peer fixture')
    return tail_expected('clip-'+case[-1])


def ticks(case):
    return (0,16,32,48,64,80,96,112) if case.startswith('clip') else (0,16,32,48,72,88,104,120) if case[-1]=='2' else (0,16,40,56,72,88,104,120)


def build(articulation=127,case='clip-2'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))
