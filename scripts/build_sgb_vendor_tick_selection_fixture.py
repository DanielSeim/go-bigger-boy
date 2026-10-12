#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored short-note phase witnesses, a long tail, then a second selection."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank,payload,assets
from build_sgb_score_atomic_fixture import build_cartridge
from build_sgb_vendor_tick_fixture import TEMPO_POINTS

PREFIX=(0xA4,0xA3,0xA4,0xA3)


def build(tempo=45,state='active',*,holdout=False,live_tempo=None,change_after=1,request_gap=None,boundary=False,selection_spin=0):
    if type(tempo) is not int or tempo not in TEMPO_POINTS or state not in ('active','completed') or type(holdout) is not bool:
        raise ValueError('invalid tick-selection controls')
    if live_tempo is not None and (type(live_tempo) is not int or live_tempo not in TEMPO_POINTS):
        raise ValueError('invalid live tempo')
    if type(change_after) is not int or change_after not in (1,3) or live_tempo is None and change_after!=1:
        raise ValueError('invalid tempo change position')
    if type(boundary) is not bool or boundary and (live_tempo is None or change_after!=3 or state!='active'):
        raise ValueError('invalid boundary fixture')
    if type(selection_spin) is not int or not 0<=selection_spin<=2499 or selection_spin and (not boundary or holdout):
        raise ValueError('invalid boundary spin')
    if request_gap is not None and (type(request_gap) is not int or request_gap not in (range(3,13) if boundary else (16,20)) or state!='active'):
        raise ValueError('invalid active request gap')
    score=bytearray(bank(echo=False))
    header=lambda t:bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,t))
    first=header(tempo)+bytes((2,125))
    for i,note in enumerate(PREFIX):
        if live_tempo is not None and i==change_after: first+=bytes((0xE7,live_tempo))
        first+=bytes((note,))
    first+=bytes((96,0xA2,0))
    second=header(45)+bytes((96,125,0x98 if boundary else 0xA3,0))
    for offset,stream in ((0x60,first),(0x90,second)):
        score[offset:offset+48]=stream+bytes(48-len(stream))
    frames=(payload(((0x2B00,bytes(score)),)),payload(assets(descriptor=(2,255,224,184,2,176))))
    gap=(request_gap if request_gap is not None else 16 if state=='active' else 124)+(2 if holdout else 0)
    return build_cartridge(frames,((4,bytes((0x49,)),1),
        (18 if holdout else 16,bytes((0x41,0,0,0,1)),None),(gap,bytes((0x41,0,0,0,2)),None)),
        spin_delays=[0,0,1536] if holdout else [0,0,selection_spin] if selection_spin else None)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,default=45)
    parser.add_argument('--state',choices=('active','completed'),default='active')
    parser.add_argument('--holdout',action='store_true')
    parser.add_argument('--live-tempo',type=int,choices=TEMPO_POINTS)
    parser.add_argument('--change-after',type=int,choices=(1,3),default=1)
    parser.add_argument('--request-gap',type=int,choices=(*range(3,13),16,20))
    parser.add_argument('--boundary',action='store_true')
    parser.add_argument('--selection-spin',type=int,default=0)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.tempo,args.state,holdout=args.holdout,live_tempo=args.live_tempo,change_after=args.change_after,request_gap=args.request_gap,boundary=args.boundary,selection_spin=args.selection_spin)
        with args.output.open('xb') as stream: stream.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
