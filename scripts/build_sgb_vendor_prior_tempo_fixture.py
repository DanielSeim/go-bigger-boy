#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Change only song 1's tempo before selecting the unchanged songs 2 and 3."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_dispatch_fixture import build as dispatch, STATES
from build_sgb_vendor_handoff_fixture import CONTROLS

TEMPOS=(45,96)
SCHEDULES=('baseline','spacing-3')
PROFILES=('base','stop-both')


def request_frames(schedule,state):
    if schedule not in SCHEDULES or state not in STATES:
        raise ValueError('invalid prior-tempo schedule/state')
    spacing=(16 if state=='active' else 100)+(3 if schedule=='spacing-3' else 0)
    return 16,spacing,spacing


def build(tempo=45,schedule='baseline',profile='base',state='active'):
    if type(tempo) is not int or tempo not in TEMPOS or profile not in PROFILES:
        raise ValueError('invalid prior-tempo control')
    return dispatch('change',state,effects=CONTROLS[profile],
        request_frames=request_frames(schedule,state),track_tempos=(tempo,45,45))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tempo',type=int,choices=TEMPOS,default=45)
    parser.add_argument('--schedule',choices=SCHEDULES,default='baseline')
    parser.add_argument('--profile',choices=PROFILES,default='base')
    parser.add_argument('--state',choices=STATES,default='active')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.tempo,args.schedule,args.profile,args.state)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
