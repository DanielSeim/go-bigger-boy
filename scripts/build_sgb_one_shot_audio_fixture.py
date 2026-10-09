#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned four-block transient beside a looping voice and left-routed GB pulse."""
import struct
from build_sgb_polyphony_audio_fixture import objects as looping_objects, PROFILES
from build_sgb_score_atomic_fixture import build_cartridge


def objects(profile='both',held_voice=2):
    score,original=looping_objects('both',held_voice)
    if profile not in PROFILES:raise ValueError('requires source profile')
    asset=bytearray(original)
    peer=5-held_voice
    slot=peer-2;start=64+64*slot
    struct.pack_into('<HH',asset,8+4*slot,0x5000+start,0x5000+start)
    asset[16+slot]=4;asset[19+4*slot]=1
    asset[start:start+64]=bytes(64)
    # Independently authored bipolar pulse with a descending amplitude in each
    # successive block. Filter zero/range eleven, ending without a loop.
    for block,amplitude in enumerate((7,5,3,1)):
        offset=start+block*9
        asset[offset]=0xB1 if block==3 else 0xB0
        values=[-amplitude]*8+[amplitude]*8
        for i in range(8):asset[offset+1+i]=((values[2*i]&15)<<4)|(values[2*i+1]&15)
    for voice in (2,3):
        if profile=='both' or profile==f'voice{voice}':continue
        slot=voice-2
        base=struct.unpack_from('<H',asset,8+4*slot)[0]-0x5000
        for block in range(asset[16+slot]):asset[base+9*block+1:base+9*block+9]=bytes(8)
    return score,bytes(asset)


def build(profile='both',held_voice=2):
    score,asset=objects(profile,held_voice)
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    return build_cartridge([payload+bytes(4096-len(payload))],
                          [(64,bytes((0x41,0,0,0,1)),None),(64,bytes((0x41,0,0,0,128)),None)],
                          io_writes=[initial,[(0x25,0),(0x80,2)]])


if __name__=='__main__':
    import argparse
    import hashlib
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--held-voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.profile,args.held_voice)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
