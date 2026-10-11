#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned SOUND effect-stop controls with an unchanged distinct-note music bank."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_dispatch_fixture import build as dispatch, STATES

CONTROLS = {'base':(0,0), 'stop-a':(128,0), 'stop-b':(0,128), 'stop-both':(128,128)}
PROFILES = tuple(CONTROLS)


def build(profile='base', state='active'):
    if profile not in PROFILES or state not in STATES:
        raise ValueError('invalid handoff profile/state')
    return dispatch('change',state,effects=CONTROLS[profile])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile',choices=PROFILES,default='base')
    parser.add_argument('--state',choices=STATES,default='active')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        image = build(args.profile,args.state)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
