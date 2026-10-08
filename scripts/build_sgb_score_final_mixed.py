#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build measured held-127 voice release through returning articulation-63 rests."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_final_return import source as prior_source


def source():
    text=prior_source()
    def asm(name):return (ROOT/('firmware/sgb/score_final_mixed_'+name+'.asm')).read_text()
    hooks={
        '    mov $bf, a\n    mov $c0, #$00\n':'    mov $bf, a\n    call final_mixed_art_init\n    beq final_return_art_bad\n    mov $c0, #$00\n',
        '    cmp a, $bf\n    bne final_return_art_bad\n':'    call final_mixed_art_compare\n    bne final_return_art_bad\n',
        '.org $0f70':asm('init')+'\n.org $0f70',
        '.org $1540':asm('compare')+'\n.org $1540',
        '    mov $72, #$0f\n    ret\nfinal_return_rest_short:':
            '    mov a, $7b\n    cmp a, #$3f\n    beq final_mixed_rest_long\n    mov $72, #$0f\n    ret\nfinal_mixed_rest_long:\n    mov $72, #$09\n    ret\nfinal_return_rest_short:',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('final mixed hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('final mixed image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
