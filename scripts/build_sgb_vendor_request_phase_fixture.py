#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned frame-phase controls retaining distinct notes, uploads and STOP fields."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_vendor_dispatch_fixture import build as dispatch, STATES
from build_sgb_vendor_handoff_fixture import CONTROLS

SCHEDULES=('baseline','first-1','first-2','spacing-1','spacing-2')
PROFILES=('base','stop-both')


def request_frames(schedule,state):
    if schedule not in SCHEDULES or state not in STATES:
        raise ValueError('invalid request-phase schedule/state')
    first=16;spacing=16 if state=='active' else 100
    if schedule.startswith('first-'): first+=int(schedule[-1])
    if schedule.startswith('spacing-'): spacing+=int(schedule[-1])
    return first,spacing,spacing


def build(schedule='baseline',profile='base',state='active'):
    if profile not in PROFILES: raise ValueError('invalid phase STOP control')
    return dispatch('change',state,effects=CONTROLS[profile],request_frames=request_frames(schedule,state))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--schedule',choices=SCHEDULES,default='baseline')
    parser.add_argument('--profile',choices=PROFILES,default='base')
    parser.add_argument('--state',choices=STATES,default='active')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.schedule,args.profile,args.state)
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
