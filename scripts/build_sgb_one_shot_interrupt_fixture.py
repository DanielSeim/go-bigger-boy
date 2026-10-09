#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned transient interrupted by SOUND stop, then restarted beside a loop."""
import struct
from build_sgb_one_shot_audio_fixture import objects as completion_objects, PROFILES
from build_sgb_score_atomic_fixture import build_cartridge


def objects(profile='both',held_voice=2):
    score,asset=completion_objects(profile,held_voice)
    score=bytearray(score)
    peer=5-held_voice
    start=0x100+(peer-2)*0x80
    prefix=[0xE0,2 if peer==2 else 10,0xE1,0,0xED,127]
    if peer==2:prefix += [0xE5,160,0xE7,96]
    score[start:start+64]=bytes(64)
    score[start:start+len(prefix)+4]=bytes(prefix+[64,127,0x98,0])
    return bytes(score),asset


def build(profile='both',held_voice=2):
    score,asset=objects(profile,held_voice)
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    initial=[(0x26,0),(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),(0x11,0x80),
             (0x12,0x40),(0x13,0),(0x14,0x87),(0x80,1)]
    commands=[(delay,bytes((0x41,0,0,0,song)),None) for delay,song in zip((64,2,4,32),(1,128,1,128))]
    return build_cartridge([payload+bytes(4096-len(payload))],commands,
                          io_writes=[initial,[(0x80,2)],[(0x80,3)],[(0x25,0),(0x80,4)]],
                          spin_delays=[0,1800,0,0])


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
