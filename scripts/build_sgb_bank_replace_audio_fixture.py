#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned stopped-bank replacement with a fresh waveform/map and continuous GB."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_audio_transition_fixture import objects as held_objects
from build_sgb_owned_sample_fixture import calibrated_assets
from build_sgb_score_atomic_fixture import build_cartridge

PROFILES=('both','old','new','gb')


def objects(profile='both',voice=2):
    if profile not in PROFILES:raise ValueError('requires source profile')
    result=[]
    for replacement in (False,True):
        score,asset=held_objects(voice,2,replacement)
        score=bytearray(score);asset=bytearray(asset)
        for channel in (2,3):
            start=struct.unpack_from('<H',score,0x60+2*channel)[0]-0x2B00
            score[score.index(0xE1,start,start+32)+1]=0
        if replacement:
            start=struct.unpack_from('<H',score,0x60+2*voice)[0]-0x2B00
            note=score.index(bytes((64,127,0xA1)),start,start+32)+2
            score[note]=0xA4
            # ID 2 now maps to physical source 3. Replace its moved square
            # with the independently authored calibrated triangle cycle.
            original=calibrated_assets()
            triangle=struct.unpack_from('<H',original,12)[0]-0x5000
            target=struct.unpack_from('<H',asset,12)[0]-0x5000
            asset[target:target+18]=original[triangle:triangle+18]
        if profile=='gb' or (profile=='old' and replacement) or (profile=='new' and not replacement):
            for slot in (0,1):
                start=struct.unpack_from('<H',asset,8+4*slot)[0]-0x5000
                for block in range(asset[16+slot]):asset[start+9*block+1:start+9*block+9]=bytes(8)
        result.append((bytes(score),bytes(asset)))
    return tuple(result)


def build(profile='both',voice=2):
    payloads=[]
    for score,asset in objects(profile,voice):
        data=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
        if len(data)>4096:raise ValueError('replacement upload bound')
        payloads.append(data+bytes(4096-len(data)))
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    sound=lambda song:bytes((0x41,0,0,0,song))
    return build_cartridge(payloads,[(64,sound(1),None),(12,sound(128),None),(4,bytes((0x49,)),1),
                                     (64,sound(1),None),(12,sound(128),None)],
                          io_writes=[initial,[(0x80,2)],[(0x80,3)],[(0x80,4)],[(0x25,0),(0x80,5)]])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.profile,args.voice)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
