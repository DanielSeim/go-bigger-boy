#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned uploaded instruments isolating measured voice-volume setup points."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank,payload,assets
from build_sgb_score_atomic_fixture import build_cartridge

CASES={
    'pan': ((10,160,127,127),(0,160,127,127),(20,160,127,127)),
    'volume': ((10,160,127,127),(10,80,127,127),(10,160,64,127)),
    'velocity': ((10,160,127,125),)*3,
}


def build(case='pan',holdout=False):
    if case not in CASES or type(holdout) is not bool:
        raise ValueError('invalid voice-volume fixture')
    score=bytearray(bank(echo=False))
    for i,(pan,song,track,articulation) in enumerate(CASES[case]):
        start=0x60+48*i
        stream=bytes((0xE0,2,0xE1,pan,0xE5,song,0xED,track,0xE7,
                      96 if holdout else 45,24 if holdout else 16,articulation,
                      0xA4-i if holdout else 0x98,0))
        score[start:start+48]=stream+bytes(48-len(stream))
    frames=(payload(((0x2B00,bytes(score)),)),
            payload(assets(descriptor=(2,255,224,184,2,176))))
    commands=[(4,bytes((0x49,)),1)]
    commands += [(19 if holdout else 16,bytes((0x41,0,0,0,i)),None) for i in (1,2,3)]
    return build_cartridge(frames,tuple(commands))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES,default='pan')
    parser.add_argument('--holdout',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.case,args.holdout)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
