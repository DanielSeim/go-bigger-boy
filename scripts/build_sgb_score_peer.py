#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build preserved clipped voices with inactive gate countdowns frozen."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_tail import source as prior_source


def source():
    text=prior_source()
    hooks={
        '    mov $71, #$0c\n    ret\n':'    mov $71, #$0c\n    mov $f2, #$5c\n    mov $f3, #$0c\n    ret\n',
        'poly_all:\n    mov a, $64\n    beq poly_return\n    mov $70, #$0c\n    call poly_release\n    ret\n':
            'poly_all:\n    mov $51, #$00\n    mov $7a, #$00\n    mov a, $64\n    beq poly_return\n    mov $f2, #$4c\n    mov $f3, #$00\n    ret\n',
        'reselect_pulse2:\n':'reselect_pulse2:\n    mov a, $a3\n    and a, #$04\n    beq gates_pulse3\n',
        'reselect_pulse3:\n':'reselect_pulse3:\n    mov a, $a3\n    and a, #$08\n    beq gates_expire\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('peer lifecycle hook changed')
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('peer program exceeds diagnostic bound')
    return image


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    try:
        with args.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())


if __name__=='__main__':main()
