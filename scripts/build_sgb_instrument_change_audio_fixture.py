#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned loop/one-shot/loop selection beside a held loop and GB pulse."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_one_shot_audio_fixture import objects as sample_objects
from build_sgb_score_atomic_fixture import build_cartridge


PROFILES=('both','loop','transient','gb')


def objects(profile='both',held_voice=2):
    if profile not in PROFILES:raise ValueError('requires source profile')
    # Isolate sample sources rather than voices: both voices share the loop.
    source_profile=('both' if profile=='both' else 'gb' if profile=='gb' else
                    f'voice{held_voice}' if profile=='loop' else f'voice{5-held_voice}')
    _,asset=sample_objects(source_profile,held_voice)
    score=bytearray(2048)
    struct.pack_into('<3H',score,0,0x2B20,0x2B30,0x2B40)
    for root in (0x20,0x30,0x40):
        struct.pack_into('<3H',score,root,0x2B60,0x2B70,0)
    loop_id=2 if held_voice==2 else 10
    transient_id=10 if held_voice==2 else 2
    for pattern in (0,1):
        for voice in (2,3):
            start=0x100+pattern*0x100+(voice-2)*0x80
            struct.pack_into('<H',score,0x60+pattern*16+voice*2,0x2B00+start)
            if pattern:
                stream=bytes((16,127,0xC9,0))
            else:
                prefix=[0xE0,loop_id,0xE1,0,0xED,127]
                if voice==2:prefix += [0xE5,160,0xE7,96]
                body=([64,127,0xA1,0] if voice==held_voice else
                      [16,63,0xA4,0xE0,transient_id,0xA4,0xE0,loop_id,16,127,0xA4,0])
                stream=bytes(prefix+body)
            score[start:start+len(stream)]=stream
    return bytes(score),asset


def build(profile='both',held_voice=2):
    score,asset=objects(profile,held_voice)
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    if len(payload)>4096:raise ValueError('instrument-change upload bound')
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    return build_cartridge([payload+bytes(4096-len(payload))],
                          [(64,bytes((0x41,0,0,0,1)),None),(64,bytes((0x41,0,0,0,128)),None)],
                          io_writes=[initial,[(0x25,0),(0x80,2)]])


if __name__=='__main__':
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
