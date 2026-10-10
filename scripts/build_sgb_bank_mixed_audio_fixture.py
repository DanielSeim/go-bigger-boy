#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned audible bank recovery after mixed preflight and semantic failures."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_bank_reject_audio_fixture import replacement
from build_sgb_bank_recovery_audio_fixture import pack,objects,PROFILES
from build_sgb_score_atomic_fixture import build_cartridge

ORDERS=('gap-root','root-gap')


def failures(order):
    if order not in ORDERS:raise ValueError('requires admitted mixed failure order')
    return ('asset-gap','bad-root') if order=='gap-root' else ('bad-root','asset-gap')


def build(order='gap-root',profile='both',voice=2):
    first_kind,second_kind=failures(order)
    first,fresh=objects(profile,voice)
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    commands=[(64,sound(1),None),(12,None,None),(4,bytes((0x49,)),1),(16,sound(1),None),
              (4,sound(128),None),(4,bytes((0x49,)),2),(16,sound(1),None),
              (4,bytes((0x49,)),3),(64,sound(1),None),(12,sound(128),None)]
    return build_cartridge([pack(*first),replacement(first_kind,voice),replacement(second_kind,voice),pack(*fresh)],
        commands,io_writes=[initial]+[[(0x80,i)] for i in range(2,10)]+[[(0x25,0),(0x80,10)]])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--order',choices=ORDERS,default='gap-root')
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.order,args.profile,args.voice)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
