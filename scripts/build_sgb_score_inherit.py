#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build execution-order per-channel mix inheritance in the isolated driver."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT
from build_sgb_score_reselect import source as prior_source


def source():
    text=prior_source()
    hooks={
        '    mov $81, #$0a\n    mov $82, #$7f\n':'    mov $81, #$ff\n    mov $82, #$ff\n    mov $b0, #$00\n',
        'mix_command:\n':'mix_command:\n    mov $b0, #$01\n',
        '    call mix_validate\n    call mix_cache\n':'    mov $b0, #$00\n    call mix_cache\n',
        'calls_track_end:\n':'calls_track_end:\n    mov a, $b0\n    beq inherit_track_end\n    jmp pair_reject\ninherit_track_end:\n',
        'multi_begin:\n':'multi_begin:\n    call inherit_begin\n',
        '    call track_emit\n':'    call inherit_event\n    call track_emit\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('inheritance source hook changed')
        text=text.replace(before,after)
    return text+'\n.org $1680\n'+(ROOT/'firmware/sgb/score_inherit.asm').read_text()


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('inheritance program exceeds diagnostic code bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output:output.write(image)
    except (OSError,ValueError) as error:parser.error(str(error))
    print(f'Experimental mix inheritance: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':main()
