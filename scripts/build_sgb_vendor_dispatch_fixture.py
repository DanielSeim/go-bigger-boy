#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned distinct-note music IDs for bounded host dispatch observations."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_music_fixture import bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge

PROFILES = ('change', 'repeat')
STATES = ('active', 'completed')


def build(profile='change', state='active', *, effects=(0,0), request_frames=None, track_tempos=(45,45,45)):
    if profile not in PROFILES or state not in STATES:
        raise ValueError('invalid dispatch profile/state')
    if (not isinstance(effects,tuple) or len(effects)!=2 or
            any(type(value) is not int or value not in (0,128) for value in effects)):
        raise ValueError('invalid owned effect-stop controls')
    if request_frames is None:
        request_frames = (16,16,16) if state == 'active' else (16,100,100)
    if (not isinstance(request_frames,tuple) or len(request_frames)!=3 or
            any(type(n) is not int or not 1<=n<=104 for n in request_frames)):
        raise ValueError('invalid bounded request frame counts')
    if (not isinstance(track_tempos,tuple) or len(track_tempos)!=3 or
            any(type(n) is not int or not 1<=n<=255 for n in track_tempos)):
        raise ValueError('invalid bounded track tempos')
    score = bytearray(bank(echo=False))
    for index, note in enumerate((0xA4, 0xA3, 0xA2)):
        start = 0x60 + 48*index
        track = bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,track_tempos[index],
                       96,125,note,0))
        score[start:start+48] = track + bytes(48-len(track))
    frames = (payload(((0x2B00,bytes(score)),)),
              payload(assets(descriptor=(2,255,224,184,2,176))))
    ids = (1,2,3) if profile == 'change' else (1,1,1)
    commands = [(4,bytes((0x49,)),1)]
    commands += [(delay,bytes((0x41,*effects,0,music)),None)
                 for delay,music in zip(request_frames,ids)]
    return build_cartridge(frames,tuple(commands))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=PROFILES, default='change')
    parser.add_argument('--state', choices=STATES, default='active')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.profile,args.state)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
