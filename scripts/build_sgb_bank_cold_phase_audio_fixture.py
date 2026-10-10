#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned staggered subframe SOUND commands across cold mixed one-byte recovery."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_bank_cold_mixed_audio_fixture import build as mixed_build,ORDERS,PROFILES,failures
SPINS=(0,1,623,0,1247,0,2499,31)


def build(order='gap-root',profile='both',voice=2):
    return mixed_build(order,profile,voice,semantic_tail=True,spin_delays=SPINS)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--order',choices=ORDERS,default='gap-root')
    parser.add_argument('--profile',choices=PROFILES,default='both')
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.order,args.profile,args.voice)
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
