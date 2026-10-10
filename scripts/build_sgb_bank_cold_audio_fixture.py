#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned cold rejection and first-bank recovery with isolated source controls."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_bank_reject_audio_fixture import replacement,FAULTS
from build_sgb_bank_recovery_audio_fixture import pack,objects,semantic_payload
from build_sgb_score_atomic_fixture import build_cartridge
PROFILES=('both','native','gb')


def build(kind='asset-gap',profile='both',voice=2,*,semantic_tail=False,repeat=False,mixed=False):
    if any(type(x) is not bool for x in (semantic_tail,repeat,mixed)) or (semantic_tail and kind!='bad-root' and not mixed) or (repeat and not (semantic_tail or mixed)) or (mixed and not repeat):
        raise ValueError('cold repeat requires tail or mixed failure')
    if profile not in PROFILES:raise ValueError('requires admitted cold source control')
    fresh=objects('gb' if profile=='gb' else 'both',voice)[1]
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0 if profile=='native' else 0x10),
             (0x10,0),(0x11,0x80),(0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    commands=[(64,None,None),(16,sound(1),None),(4,sound(128),None),(4,bytes((0x49,)),1),
              (64,sound(1),None),(12,sound(128),None)]
    if repeat:commands[3:3]=[(4,bytes((0x49,)),0),(16,sound(1),None)]
    count=len(commands)
    payloads=[semantic_payload(voice) if semantic_tail and kind=='bad-root' else replacement(kind,voice),pack(*fresh)]
    if mixed:
        second='bad-root' if kind=='asset-gap' else 'asset-gap'
        payloads.insert(1,semantic_payload(voice) if semantic_tail and second=='bad-root' else replacement(second,voice))
        commands[3]=(4,bytes((0x49,)),1)
        commands[5]=(4,bytes((0x49,)),2)
    return build_cartridge(payloads,commands,
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
