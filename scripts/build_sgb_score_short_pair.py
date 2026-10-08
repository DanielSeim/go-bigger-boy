#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build guarded consecutive short events after a held-voice return."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_short_continue import source as prior_source


def source():
    text=prior_source()
    hooks={
        '    mov a, $47\n    cmp a, #$80\n    bne short_return_gate_bad\n    mov a, $26\n    cmp a, #$a0\n    bne short_return_gate_bad\n':'    jmp short_pair_slot\nshort_pair_gate_profile:\n',
        'short_return_shape:\n    mov a, $4282\n':'short_return_shape:\n    mov a, $4282\n    cmp a, #$04\n    beq short_pair_shape\n',
        'short_return_shape_bad:\n':'short_pair_shape:\n    mov a, $42a2\n    cmp a, #$04\n    beq short_return_shape_ok\nshort_return_shape_bad:\n',
        'final_return_boundary:\n    mov a, $4282\n':'final_return_boundary:\n    mov a, $4282\n    cmp a, #$04\n    beq final_return_boundary_note\n',
        '.org $0e00':(ROOT/'firmware/sgb/score_short_pair_slot.asm').read_text()+'\n.org $0e00',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('short pair hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('short pair image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
