#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned 32-sample periodic assets and a bounded held-note upload fixture."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_adsr_fixture import objects
from build_sgb_score_atomic_fixture import build_cartridge


def calibrated_assets():
    """Two owned single-cycle filter-0 waves, both 32 samples per cycle.

    The C4 target is an authored convention, not an inferred vendor note map.
    The existing normal tuning selector is calibrated for this owned loop length.
    """
    _,prior=objects(ids=(2,10),tuning=(0,0),envelope_profile='second')
    data=bytearray(prior)
    square=(-7,)*16+(7,)*16
    triangle=tuple(range(-7,8))+(7,)+tuple(range(7,-8,-1))+(-7,)
    for index,wave in enumerate((square,triangle)):
        base=64+64*index
        start=base+9*(index+1)
        data[base:base+64]=bytes(64)
        data[16+index]=2
        struct.pack_into('<HH',data,8+4*index,0x5000+start,0x5000+start)
        for block in range(2):
            offset=start+9*block
            data[offset]=0xB3 if block else 0xB0
            for pair in range(8):
                left,right=wave[block*16+pair*2:block*16+pair*2+2]
                data[offset+1+pair]=((left&15)<<4)|(right&15)
    return bytes(data)


def build(note=24):
    if type(note) is not int or not 24<=note<=36:
        raise ValueError('requires owned base note 24..36')
    score,_=objects(ids=(2,10),score_profile='held',tuning=(0,0),envelope_profile='second')
    score=bytearray(score)
    for voice in (2,3):
        start=struct.unpack_from('<H',score,0x60+2*voice)[0]-0x2B00
        index=score.index(0x98,start,start+32)
        score[index]=0x80+note
    asset=calibrated_assets()
    payload=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    if len(payload)>4096:
        raise ValueError('owned sample payload exceeds transfer frame')
    return build_cartridge([payload+bytes(4096-len(payload))],[(64,bytes((0x41,0,0,0,1)),None)])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--note',type=int,default=24)
    args=parser.parse_args()
    try:
        image=build(args.note)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
