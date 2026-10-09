#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned D8 asynchronous score envelopes with fixed looping BRR slots."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_async_envelope_fixture import CASES, peer_notes
from build_sgb_score_adsr_fixture import objects as adsr_objects
from build_sgb_score_atomic_fixture import build_cartridge


def objects(case='retrigger',held_voice=2,slots=(2,3)):
    notes = peer_notes(case)
    if type(held_voice) is not int or held_voice not in (2,3) or not isinstance(slots,(tuple,list)) or (
            len(slots)!=2 or any(type(slot) is not int for slot in slots) or set(slots)!={2,3}):
        raise ValueError('requires held voice 2/3 and a permutation of slots 2/3')
    ids = (10,2) if slots[0]==2 else (2,10)
    _,assets = adsr_objects(ids,tuning=(2,0) if slots[0]==2 else (0,2),
                           envelope_profile='first' if slots[0]==2 else 'second')
    score = bytearray(2048)
    struct.pack_into('<3H',score,0,0x2B20,0x2B30,0x2B40)
    for root in (0x20,0x30,0x40):
        struct.pack_into('<3H',score,root,0x2B60,0 if case=='clipped' else 0x2B70,0)
    for pattern in (0,1):
        for voice in (2,3):
            start=0x100+pattern*0x100+(voice-2)*0x80
            struct.pack_into('<H',score,0x60+pattern*16+voice*2,0x2B00+start)
            stream=bytearray()
            if pattern==1:
                stream.extend((16,127,0xC9,0))
            else:
                stream.extend((0xE0,10,0xE1,10,0xED,127))
                if voice==2:
                    stream.extend((0xE5,160,0xE7,96))
                if voice==held_voice:
                    stream.extend((64,127,0x98,0))
                else:
                    stream.extend((16,127))
                    for index,(instrument,note) in enumerate(notes):
                        if case=='switch' and index:
                            stream.extend((0xE0,instrument))
                        stream.append(0x80+note)
                        if case=='rests':
                            stream.append(0xC9)
                    if case=='clipped':
                        stream.append(0xC9)
                    stream.append(0)
            score[start:start+len(stream)]=stream
    return bytes(score),assets


def build(case='retrigger',held_voice=2,slots=(2,3)):
    score,assets = objects(case,held_voice,slots)
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(assets),0x5000)+assets+struct.pack('<HH',0,0x0400)
    if len(payload)>4096:
        raise ValueError('asynchronous envelope payload exceeds transfer frame')
    return build_cartridge([payload+bytes(4096-len(payload))],[(64,bytes((0x41,0,0,0,1)),None)])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES,default='retrigger')
    parser.add_argument('--held-voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--slots',default='2,3')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.case,args.held_voice,tuple(int(value) for value in args.slots.split(',')))
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
