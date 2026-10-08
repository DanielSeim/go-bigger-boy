#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build ready boundary notes keyed through an immediate following rest."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_pending import source as prior_source


def source():
    text=prior_source()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $b8, #$00\n    mov $b9, #$00\n',
        '    cmp a, #$c9\n    beq reverse_bad\n':
            '    cmp a, #$c9\n    beq follow_rest_guard_jump\n',
        'pending_guard_jump:\n': 'follow_rest_guard_jump:\n    jmp follow_rest_guard\npending_guard_jump:\n',
        '    call gates_event\n    call pending_mix\n':
            '    call follow_rest_gate\n    call pending_mix\n',
        '    mov $72, a\n    ret\npending_bad:\n':
            '    mov $72, a\n    mov $b9, a\n    ret\npending_bad:\n',
        '    call poly_on\n    mov $b6, #$00\n':
            '    call poly_on\n    mov $b6, #$00\n    mov $b8, #$00\n    mov $b9, #$00\n',
        '.org $1300':(ROOT/'firmware/sgb/score_follow_rest.asm').read_text()+'\n.org $1300',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('following-rest hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('following-rest program exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
