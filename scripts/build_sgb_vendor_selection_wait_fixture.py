#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored preceding-tempo pacing controls and interruption fixtures."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank,payload,assets
from build_sgb_score_atomic_fixture import build_cartridge

TEMPOS=(32,45,64,96,128)
PROFILES=('normal','rapid','stop','upload')


def build(tempo=45,state='active',holdout=True,profile='normal',*,request_offset=4,first_duration=None,
          first_request_offset=0,selection_spin=0):
    if type(tempo) is not int or tempo not in (*TEMPOS,99,112) or state not in ('active','completed') or type(holdout) is not bool or profile not in PROFILES:
        raise ValueError('invalid selection-wait fixture')
    if profile!='normal' and (state!='active' or tempo!=32):
        raise ValueError('interruptions use active tempo 32')
    if type(request_offset) is not int or not 0<=request_offset<=4:
        raise ValueError('invalid bounded request offset')
    if first_duration is None:
        first_duration=48 if state=='completed' else 96
    if type(first_duration) is not int or first_duration not in (24,48,96):
        raise ValueError('invalid authored first duration')
    if type(first_request_offset) is not int or not 0<=first_request_offset<=4:
        raise ValueError('invalid bounded first request offset')
    if type(selection_spin) is not int or not 0<=selection_spin<=2499:
        raise ValueError('invalid bounded selection spin')
    if profile!='normal' and (first_request_offset or selection_spin):
        raise ValueError('phase controls require normal profile')
    score=bytearray(bank(echo=False))
    for i,note in enumerate((0xA4,0xA3,0xA2)):
        stream=bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,tempo if i==0 else 45,
                      first_duration if i==0 else 96,125,note,0))
        score[0x60+48*i:0x90+48*i]=stream+bytes(48-len(stream))
    frames=(payload(((0x2B00,bytes(score)),)),payload(assets(descriptor=(2,255,224,184,2,176))))
    sound=lambda music:bytes((0x41,0,0,0,music))
    gap=(16 if state=='active' else 100)+(request_offset if holdout else 0)
    commands=[(4,bytes((0x49,)),1),(16+first_request_offset,sound(1),None)]
    if profile=='normal': commands += [(gap,sound(2),None),(gap,sound(3),None)]
    elif profile=='rapid': commands += [(1,sound(2),None),(1,sound(3),None)]
    elif profile=='stop': commands += [(1,sound(2),None),(1,sound(128),None),(16,sound(3),None)]
    else: commands += [(1,sound(2),None),(1,bytes((0x49,)),None),(16,sound(3),None)]
    return build_cartridge(frames,tuple(commands),spin_delays=[0,0,selection_spin,0] if selection_spin else None)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tempo',type=int,choices=(*TEMPOS,99,112),default=45)
    parser.add_argument('--state',choices=('active','completed'),default='active')
    parser.add_argument('--baseline',action='store_true')
    parser.add_argument('--profile',choices=PROFILES,default='normal')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.tempo,args.state,not args.baseline,args.profile)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
