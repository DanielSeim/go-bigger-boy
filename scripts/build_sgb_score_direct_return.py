#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the guarded measured held-voice direct-note return diagnostic."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_final_mixed import source as prior_source


def source():
    text=prior_source()
    def asm(name):return (ROOT/('firmware/sgb/score_direct_return_'+name+'.asm')).read_text()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $c3, #$00\n',
        '    mov a, $4280\n    cmp a, #$04\n    beq final_return_rest_ok\n    cmp a, #$08\n    bne final_return_miss\n':
            '    call direct_return_shape\n    beq final_return_miss\n    mov a, $4280\n',
        '    mov a, $4281\n    cmp a, #$c9\n    bne final_return_miss\n':'',
        'final_return_miss:\n':'final_return_miss:\n    mov $c3, #$00\n',
        '    cmp a, #$a0\n    bne final_return_boundary_bad\n':'    call direct_return_pending_compare\n    bne final_return_boundary_bad\n',
        '    call pending_mix\n    call duet_voice\n':'    call direct_return_voice\n',
        'sparse_start_on:\n    call poly_on\n':'sparse_start_on:\n    call direct_return_on\n',
        '.org $1000':asm('on')+'\n.org $1000',
        '.org $1100':asm('shape')+'\n.org $1100',
        '.org $1580':asm('pending')+'\n.org $1580',
        '.org $1700':asm('voice')+'\n.org $1700',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('direct-return hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('direct-return image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
