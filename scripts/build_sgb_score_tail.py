#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build native channel-priority trailing pan/track-volume controls."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT
from build_sgb_score_timing import source as prior_source


def source():
    text=prior_source()
    hooks={
        '    mov a, $b0\n    beq inherit_track_end\n    jmp pair_reject\ninherit_track_end:\n':'    call mix_cache\n',
        'multi_advance:\n':'multi_advance:\n    call tail_end\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('trailing control hook changed')
        text=text.replace(before,after)
    return text+'\n.org $1790\n'+(ROOT/'firmware/sgb/score_tail.asm').read_text()


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('trailing control program exceeds diagnostic bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(f'Experimental trailing controls: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':main()
