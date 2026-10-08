#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build guarded returning-rest release followed by a pending final note/rest."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_final_peer import source as prior_source


def source():
    text=prior_source()
    def asm(name):return (ROOT/('firmware/sgb/score_final_return'+name+'.asm')).read_text()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $be, #$00\n    call final_return_profile\n',
        '    call follow_rest_gate\n    call pending_mix\n':
            '    call follow_rest_gate\n    call final_return_rest_gate\n    call pending_mix\n',
        '    cmp a, #$01\n    bne final_bad\n    mov a, $61\n':
            '    cmp a, #$01\n    beq final_return_single\n    mov a, $be\n    beq final_bad\n    mov a, $61\n    cmp a, #$82\n    bne final_bad\n    mov a, $62\n    cmp a, #$a2\n    bne final_bad\n    bra final_return_accept\nfinal_return_single:\n    mov a, $61\n',
        '    mov $ba, #$01\n    mov $b5, #$01\n':
            'final_return_accept:\n    mov $ba, #$01\n    mov $b5, #$01\n',
        '.org $0f40':asm('_helpers')+'\n.org $0f40',
        '.org $0f80': '.org $0f70\nfinal_return_art_offsets:\n.byte $00, $02, $20, $22, $60, $62, $80, $82, $a0\n.org $0f80',
        '.org $1300':asm('')+'\n.org $1300',
        '.org $1400':asm('_rest')+'\n.org $1400',
        '.org $1480':asm('_geometry')+'\n.org $1480',
        '.org $1580':asm('_boundary')+'\n.org $1580',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('final return hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('final return image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
