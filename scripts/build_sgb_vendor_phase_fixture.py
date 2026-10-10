#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned long note/rest chains and repeated selections; no original assets."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge

CASES = ('long-chain', 'long-rest-chain', 'short-chain', 'short-rest-chain',
         'mixed-chain', 'selection', 'selection-short', 'selection-completed')


def sequence(case):
    if case not in CASES:
        raise ValueError('unknown phase fixture case')
    if case == 'long-chain': return 45, [(96,125,36)]*8
    if case == 'long-rest-chain': return 45, [(96,125,36),(1,None,None)]*8
    if case == 'short-chain': return 96, [(16,127,24+i%14) for i in range(24)]
    if case == 'short-rest-chain':
        return 96, [event for i in range(12) for event in ((16,127,24+i%14),(1,None,None))]
    if case == 'mixed-chain':
        return 45, [(96,125,36),(1,None,None),(16,127,24),(3,None,None)]*4
    return (96, [(16,127,36)]) if case == 'selection-short' else (45, [(96,125,36)])


def build(case='long-rest-chain'):
    tempo, events = sequence(case)
    score = bytearray(bank(echo=False))
    # Song 1 owns the remaining track pool; songs 2/3 are never selected.
    score[0x60:] = bytes(160)
    track = bytearray((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,tempo))
    for duration, articulation, note in events:
        track.append(duration)
        if articulation is not None: track.append(articulation)
        track.append(0xC9 if note is None else 0x80+note)
    track.append(0)
    if len(track) > 160: raise ValueError('owned track exceeds its pool')
    score[0x60:0x60+len(track)] = track
    frames = (payload(((0x2B00,bytes(score)),)),
              payload(assets(descriptor=(2,255,224,184,2,176))))
    sound = bytes((0x41,0,0,0,1))
    commands = [(4,bytes((0x49,)),1),(16,sound,None)]
    if case.startswith('selection'):
        gaps = (100,101,102) if case == 'selection-completed' else (8,9,10,11,12,16,24,100)
        commands.extend((gap,sound,None) for gap in gaps)
    return build_cartridge(frames,tuple(commands))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES,default='long-rest-chain')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        image = build(args.case)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
