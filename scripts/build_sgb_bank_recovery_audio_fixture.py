#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned audible rejection, blocked SOUND and changed-bank retry."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_bank_reject_audio_fixture import replacement,FAULTS
from build_sgb_bank_replace_audio_fixture import objects,PROFILES
from build_sgb_score_atomic_fixture import build_cartridge


def pack(score,asset):
    data=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    if len(data)>4096:raise ValueError('recovery upload bound')
    return data+bytes(4096-len(data))


def build(kind='asset-gap',profile='both',voice=2,*,semantic_tail=False,repeat=False):
    if any(type(x) is not bool for x in (semantic_tail,repeat)) or (semantic_tail and kind!='bad-root') or (repeat and not semantic_tail):
        raise ValueError('tail requires semantic failure; repeat requires tail')
    first,fresh=objects(profile,voice)
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    bad=replacement(kind,voice)
    if semantic_tail:
        score,asset=objects('both',voice)[1]
        score=bytes(2)+score[2:]
        data=(struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',191,0x5000)+asset[:191]+
              struct.pack('<HH',1,0x50BF)+asset[191:]+struct.pack('<HH',0,0x0400))
        bad=data+bytes(4096-len(data))
    commands=[(64,sound(1),None),(12,None,None),(4,bytes((0x49,)),1),(16,sound(1),None),
         (4,sound(128),None),(4,bytes((0x49,)),2),(64,sound(1),None),(12,sound(128),None)]
    if repeat:commands[5:5]=[(4,bytes((0x49,)),1),(16,sound(1),None)]
    count=len(commands)
    return build_cartridge([pack(*first),bad,pack(*fresh)],commands,
        io_writes=[initial]+[[(0x80,i)] for i in range(2,count)]+[[(0x25,0),(0x80,count)]])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind',choices=FAULTS,default='asset-gap')
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--semantic-tail',action='store_true')
    parser.add_argument('--repeat',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.kind,args.profile,args.voice,semantic_tail=args.semantic_tail,repeat=args.repeat)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
