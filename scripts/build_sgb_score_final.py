#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build measured final-ready note/rest stop with bounded held-note observation."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_follow_rest import source as prior_source


def source():
    text=prior_source()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $ba, #$00\n',
        '    cmp a, $46\n    bcs reverse_bad\n':
            '    cmp a, $46\n    bcs final_guard_jump\n',
        'follow_rest_guard_jump:\n':'final_guard_jump:\n    jmp final_guard\nfollow_rest_guard_jump:\n',
        '    call poly_complete\n    mov $f1, #$80\n':
            '    call final_complete\n    mov $f1, #$80\n',
        '.org $1400':(ROOT/'firmware/sgb/score_final_complete.asm').read_text()+'\n.org $1400',
        '.org $1680':(ROOT/'firmware/sgb/score_final.asm').read_text()+'\n.org $1680',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('final boundary hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('final program exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
