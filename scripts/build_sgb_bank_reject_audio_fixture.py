#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned audible loop followed by rejected upload and blocked SOUND attempts."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_bank_replace_audio_fixture import objects
from build_sgb_score_atomic_fixture import build_cartridge

FAULTS=('asset-gap','bad-root')
PROFILES=('both','gb')


def replacement(kind,voice=2):
    if kind not in FAULTS:raise ValueError('requires admitted rejection kind')
    score,asset=objects('both',voice)[1]
    if kind=='bad-root':
        score=bytes(2)+score[2:]
        chunks=((0x2B00,score),(0x5000,asset))
    else:
        chunks=((0x2B00,score),(0x5000,asset[:20]),(0x5015,asset[21:]))
    data=b''.join(struct.pack('<HH',len(value),address)+value for address,value in chunks)
    data+=struct.pack('<HH',0,0x0400)
    return data+bytes(4096-len(data))


def build(kind='asset-gap',profile='both',voice=2):
    if profile not in PROFILES:raise ValueError('requires owned or silent old-bank control')
    score,asset=objects(profile,voice)[0]
    first=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    return build_cartridge([first+bytes(4096-len(first)),replacement(kind,voice)],
        [(64,sound(1),None),(12,None,None),(4,bytes((0x49,)),1),(64,sound(1),None),(12,sound(128),None)],
        io_writes=[initial,[(0x80,2)],[(0x80,3)],[(0x80,4)],[(0x25,0),(0x80,5)]])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind',choices=FAULTS,default='asset-gap')
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.kind,args.profile,args.voice)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
