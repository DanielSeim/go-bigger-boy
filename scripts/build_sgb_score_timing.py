#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build bounded per-channel duration/articulation inheritance."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT
from build_sgb_score_inherit import source as prior_source


def source():
    text=prior_source()
    hooks={
        'phrase_begin:\n':'phrase_begin:\n    call timing_init\n',
        '    mov $68, #$00\n    mov $69, #$00\n':'    call timing_track\n',
        '    inc $46\n    jmp phrase_pattern\n':'    call timing_pattern\n    inc $46\n    jmp phrase_pattern\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('timing inheritance source hook changed')
        text=text.replace(before,after)
    extra=(ROOT/'firmware/sgb/score_timing.asm').read_text()
    prefix,suffix=extra.split('.org $1700\n')
    if text.count('.org $1680\n')!=1:raise ValueError('timing code placement changed')
    return text.replace('.org $1680\n',prefix+'\n.org $1680\n')+'\n.org $1700\n'+suffix


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('timing inheritance exceeds diagnostic code bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(f'Experimental timing inheritance: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':main()
