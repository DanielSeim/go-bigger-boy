#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned held tone, real SOUND lifecycle and phase-preserving GB routing writes."""
import struct
from build_sgb_host_sample_pitch_fixture import objects as octave
from build_sgb_score_atomic_fixture import build_cartridge

PROFILES=('toggle','silent','on')
DELAYS=(64,8,4,4,6,12)
SONGS=(1,None,None,128,1,128)
ROUTES=(0x10,0,0x10,0x10,0x10,0)


def objects(voice=2,instrument=2,reversed_map=False):
    score,asset=octave(voice,instrument,reversed_map)
    score=bytearray(score)
    for channel in (2,3):
        start=struct.unpack_from('<H',score,0x60+2*channel)[0]-0x2B00
        body=score.index(bytes((16,127)),start,start+32)
        score[body:body+10]=bytes((64,127,0xA1 if channel==voice else 0xC9,0))+bytes(6)
    return bytes(score),asset


def build(profile='toggle',voice=2,instrument=2,reversed_map=False):
    if profile not in PROFILES:
        raise ValueError('unknown GB routing profile')
    score,asset=objects(voice,instrument,reversed_map)
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    if len(payload)>4096:
        raise ValueError('transition payload bound')
    writes=[]
    for stage,route in enumerate(ROUTES,1):
        route=route if profile=='toggle' else 0x10 if profile=='on' else 0
        init=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x10,0),(0x11,0x80),
              (0x12,0x40),(0x13,0),(0x14,0x87)] if stage==1 else []
        writes.append(init+[(0x25,route),(0x80,stage)])
    commands=[(delay,bytes((0x41,0,0,0,song)) if song is not None else None,None) for delay,song in zip(DELAYS,SONGS)]
    return build_cartridge([payload+bytes(4096-len(payload))],commands,io_writes=writes)


if __name__=='__main__':
    import argparse
    import hashlib
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=PROFILES,default='toggle')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
