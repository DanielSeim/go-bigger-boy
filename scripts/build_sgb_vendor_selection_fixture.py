#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned constant-note controls isolating selection workload and request phase."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge

PROFILES = ('base','prefix16','prefix64','tail16','tail64','phase625','phase1250','phase1875')
STATES = ('active','completed')


def build(profile='base', state='active'):
    if profile not in PROFILES or state not in STATES:
        raise ValueError('invalid selection isolation profile/state')
    score = bytearray(bank(echo=False)); score[0x60:] = bytes(160)
    controls = bytes((0xED,127))*(int(profile[6:]) if profile.startswith('prefix') else
                                 int(profile[4:]) if profile.startswith('tail') else 0)
    track = bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,45))
    if profile.startswith('prefix'): track += controls
    track += bytes((96,125,0xA4))
    if profile.startswith('tail'): track += controls
    track += bytes((0,))
    if len(track)>160: raise ValueError('selection track exceeds owned pool')
    score[0x60:0x60+len(track)] = track
    frames = (payload(((0x2B00,bytes(score)),)),payload(assets(descriptor=(2,255,224,184,2,176))))
    sound = bytes((0x41,0,0,0,1))
    # Leave room for the slow original SGB1 first selection plus prefix parsing.
    # Eight frames can deliver request 2 before request 1 has keyed on, making
    # latest-delivery attribution ambiguous. Sixteen still interrupts this note.
    commands = ((4,bytes((0x49,)),1),(16,sound,None),(16 if state=='active' else 100,sound,None))
    spins = int(profile[5:]) if profile.startswith('phase') else 0
    return build_cartridge(frames,commands,spin_delays=(0,0,spins))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=PROFILES,default='base')
    parser.add_argument('--state',choices=STATES,default='active')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        image = build(args.profile,args.state)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
