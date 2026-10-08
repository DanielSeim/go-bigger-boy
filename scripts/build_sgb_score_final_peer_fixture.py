#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned final clipping and permanently inactive peer fixtures."""
import struct
from build_sgb_score_tail_fixture import bank as tail_bank
from build_sgb_song_selection_fixture import build_cartridge

CASES=('clip-2','clip-3','inactive-2','inactive-3')


def bank(articulation=127,case='clip-2'):
    if type(articulation) is not int or articulation not in (63,127) or case not in CASES:
        raise ValueError('invalid final peer fixture')
    ending=int(case[-1])
    data=bytearray(tail_bank(articulation,'clip-'+str(ending)))
    if case.startswith('clip'):
        struct.pack_into('<H',data,0x101,0)
    else:
        # The ending channel remains active; its clipped peer never returns.
        data[0x3FB:0x40B]=bytes(16)
        struct.pack_into('<H',data,0x3FB+2*ending,0x31FD)
        stream=bytes((16,articulation,0x9A,0x9B,0))
        data[0x6FD:0x6FD+len(stream)]=stream
        struct.pack_into('<H',data,0x103,0)
    return bytes(data)


def build(articulation=127,case='clip-2'):
    data=bank(articulation,case)
    payload=struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))


def expected(case):
    if case not in CASES:raise ValueError('invalid final peer case')
    ending=int(case[-1]);result=[]
    for index,pitch in enumerate((1068,1132,1200,1272) if case.startswith('inactive') else (1068,1132)):
        voices=(2,3) if index<2 else (ending,)
        result.append({'mask':sum(1<<v for v in voices),'voices':[
            {'voice':v,'pitch':pitch,'volumes':[1,1] if index>=2 else [7,7],
             'srcn':2,'adsr1':143,'adsr2':111,'gain':184} for v in voices]})
    return result
