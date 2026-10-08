#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build native instrument-2 prefix reselection with actual DSP setup writes."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT
from build_sgb_score_sparse import source as sparse_source


def source():
    text=sparse_source()
    hooks={
        'mix_track:\n':'mix_track:\n    mov $a4, #$00\n',
        '    beq mix_instrument\n':'    beq reselect_instrument_jump\n',
        'mix_instrument:\n':'reselect_instrument_jump:\n    jmp reselect_instrument\nmix_instrument:\n',
        'calls_timed:\n':'calls_timed:\n    call reselect_cache\n    mov a, $26\n',
        'multi_event:\n':'multi_event:\n    call reselect_event\n',
        'gates_init:\n':'gates_init:\n    mov $a8, #$00\n    mov $a9, #$00\n',
        'gates_cancel:\n':'gates_cancel:\n    call reselect_cancel\n',
        'gates_on:\n':'gates_on:\n    call reselect_pending\n',
        'gates_pulse:\n    mov $75, #$00\n':
            'gates_pulse:\n    mov $75, #$00\n    mov a, $a8\n    beq reselect_pulse2\n    dec $a8\n    bra gates_pulse3\nreselect_pulse2:\n',
        'gates_pulse3:\n':
            'gates_pulse3:\n    mov a, $a9\n    beq reselect_pulse3\n    dec $a9\n    jmp gates_expire\nreselect_pulse3:\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1: raise ValueError('instrument reselection hook changed')
        text=text.replace(before,after)
    return text+'\n.org $1540\n; Owned instrument-2 descriptor: SRCN/ADSR1/ADSR2/GAIN.\n.byte $02, $8f, $6f, $b8\n.org $1580\n'+(ROOT/'firmware/sgb/score_reselect.asm').read_text()


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096: raise ValueError('reselection program exceeds diagnostic code bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(f'Experimental instrument reselection: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__': main()
