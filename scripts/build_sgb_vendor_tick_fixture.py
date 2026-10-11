#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Eight authored short notes expose software music-tick timing via DSP writes."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank,payload,assets
from build_sgb_score_atomic_fixture import build_cartridge
from build_sgb_vendor_selection_wait_fixture import TEMPOS

TEMPO_POINTS=(*TEMPOS,112)

NOTES=(0xA4,0xA3)*4
DURATION=2


def build(tempo=45,*,holdout=False):
    if type(tempo) is not int or tempo not in TEMPO_POINTS or type(holdout) is not bool:
        raise ValueError('invalid tick fixture controls')
    score=bytearray(bank(echo=False))
    stream=bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,tempo,DURATION,125,*NOTES,0))
    score[0x60:0x90]=stream+bytes(48-len(stream))
    frames=(payload(((0x2B00,bytes(score)),)),payload(assets(descriptor=(2,255,224,184,2,176))))
    return build_cartridge(frames,((4,bytes((0x49,)),1),
        (18 if holdout else 16,bytes((0x41,0,0,0,1)),None)),
        spin_delays=[0,1536] if holdout else None)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,default=45)
    parser.add_argument('--holdout',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.tempo,holdout=args.holdout)
        with args.output.open('xb') as stream: stream.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
