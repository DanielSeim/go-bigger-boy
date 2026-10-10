#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned uploaded-instrument notes for bounded gate, release and pitch checks."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge

CASES = ('entry','entry-no-rest','short','entry-chain','pitch','stop','switch','upload','retain')


def build(case='entry', *, tuning=688):
    if case not in CASES or type(tuning) is not int or not 1<=tuning<=65535:
        raise ValueError('invalid timing fixture case or tuning')
    if case=='retain':
        return build_cartridge((payload(((0x2B00,bank(echo=True)),)),payload(assets())),
            ((4,bytes((0x49,)),1),(16,bytes((0x41,0,0,0,1)),None),(36,bytes((0x41,0,0,0,0)),None)))
    duration,articulation,tempo=(16,127,96) if case in ('short','pitch') else (96,125,45)
    notes=tuple(range(24,38)) if case=='pitch' else ((36,24,25) if case=='entry-chain' else (36,))
    score=bytearray(bank(echo=False))
    track=bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,tempo,duration,articulation))+bytes(0x80+n for n in notes)
    if case not in ('entry-no-rest','pitch'): track+=bytes((1,0xC9))
    track+=bytes((0,))
    score[0x60:0x90]=bytes(48)
    score[0x60:0x60+len(track)]=track
    data=payload(assets(descriptor=(2,0xFF,0xE0,0xB8,tuning>>8,tuning&255)))
    frames=(payload(((0x2B00,bytes(score)),)),data)
    events=[(4,bytes((0x49,)),1),(16,bytes((0x41,0,0,0,1)),None)]
    if case=='stop': events.append((8,bytes((0x41,0,0,0,128)),None))
    if case=='switch': events.append((8,bytes((0x41,0,0,0,1)),None))
    if case=='upload':
        events.extend(((8,bytes((0x49,)),1),(16,bytes((0x41,0,0,0,1)),None)))
    return build_cartridge(frames,tuple(events))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES,default='entry')
    parser.add_argument('--tuning',type=int,default=688)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.case,tuning=args.tuning)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
