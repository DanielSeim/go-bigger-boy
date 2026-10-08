#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build guarded final release of an active or permanently inactive peer."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble,ROOT
from build_sgb_score_final import source as prior_source


def source():
    text=prior_source()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $bb, #$00\n',
        'poly_complete:\n    call gates_init\n':
            'poly_complete:\n    call final_peer_guard\n    call gates_init\n    mov a, $bb\n    beq final_peer_prior\n    jmp final_peer_complete\nfinal_peer_prior:\n',
        '.org $1100':(ROOT/'firmware/sgb/score_final_peer.asm').read_text()+'\n.org $1100',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('final peer hook changed: '+before)
        text=text.replace(before,after)
    return text+'\n.org $17e0\n'+(ROOT/'firmware/sgb/score_final_peer_complete.asm').read_text()


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('final peer image exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
