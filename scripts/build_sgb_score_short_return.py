#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build guarded duration-four direct notes before independently known final events."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_direct_return import source as prior_source


def source():
    text=prior_source()
    def asm(name):return (ROOT/('firmware/sgb/score_short_return_'+name+'.asm')).read_text()
    hooks={
        'phrase_begin:\n':'phrase_begin:\n    mov $c4, #$00\n',
        '    call gates_validate\n':'    call short_return_gate\n',
        '    call final_return_profile\n':'    call final_return_profile\n    call short_return_guard\n',
        '    mov a, $4280\n    cmp a, #$08\n    beq direct_return_duration\n':
            '    mov a, $4280\n    cmp a, #$04\n    beq short_return_shape_jump\n    cmp a, #$08\n    beq direct_return_duration\n',
        'direct_return_rest_shape:\n':'short_return_shape_jump:\n    jmp short_return_shape\ndirect_return_rest_shape:\n',
        '.org $1000':asm('gate')+'\n.org $1000',
        '.org $1400':asm('shape')+'\n.org $1400',
        '.org $1540':asm('guard')+'\n.org $1540',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('short-return hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('short-return image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
