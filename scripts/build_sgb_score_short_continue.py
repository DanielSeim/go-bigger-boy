#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build guarded short returns with ordinary continuation and deferred mix."""
import argparse,hashlib,re
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_short_return import source as prior_source


def source():
    text=prior_source()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $c5, #$00\n',
        'final_return_miss:\n':'final_return_miss:\n    mov $c5, #$00\n',
        '    mov a, $42a2\n    bne final_return_boundary_bad\n':'    call short_continue_boundary\n    beq final_return_boundary_bad\n',
        'multi_tick_done:\n    call poly_on\n':'multi_tick_done:\n    call direct_return_on\n',
        '    call final_complete\n':'    call short_continue_complete\n',
        '    cmp a, #$a0\n    bne direct_return_voice_old\n':'    cmp a, #$a0\n    beq short_continue_voice\n    mov a, $c5\n    beq direct_return_voice_old\nshort_continue_voice:\n',
        '.org $1540\n; Owned instrument-2 descriptor:':'owned_instrument_descriptor:\n; Owned instrument-2 descriptor:',
        '    mov a, $1540+x\n':'    mov a, owned_instrument_descriptor+x\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('short continuation hook changed: '+before)
        text=text.replace(before,after)
    # Pack only label-addressed code after the fixed pitch/articulation tables.
    # The descriptor above is explicitly relocated by label; earlier tables stay fixed.
    text=re.sub(r'^\.org \$(?:1100|1300|1400|1480|1580|1610|1680|1700|1790|17b0|17e0)\n','',text,flags=re.M)
    for name in ('boundary','complete'):
        text+='\n'+(ROOT/('firmware/sgb/score_short_continue_'+name+'.asm')).read_text()
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('short continuation image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
