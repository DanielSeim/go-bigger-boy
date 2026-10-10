#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned cold rejection and first-bank recovery with isolated source controls."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_bank_reject_audio_fixture import replacement,FAULTS
from build_sgb_bank_recovery_audio_fixture import pack,objects
from build_sgb_score_atomic_fixture import build_cartridge
PROFILES=('both','native','gb')


def build(kind='asset-gap',profile='both',voice=2):
    if profile not in PROFILES:raise ValueError('requires admitted cold source control')
    fresh=objects('gb' if profile=='gb' else 'both',voice)[1]
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0 if profile=='native' else 0x10),
             (0x10,0),(0x11,0x80),(0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    return build_cartridge([replacement(kind,voice),pack(*fresh)],
        [(64,None,None),(16,sound(1),None),(4,sound(128),None),(4,bytes((0x49,)),1),
         (64,sound(1),None),(12,sound(128),None)],
        io_writes=[initial]+[[(0x80,i)] for i in range(2,6)]+[[(0x25,0),(0x80,6)]])


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
