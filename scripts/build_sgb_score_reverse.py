#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build channel-2 execution before a channel-3 end and immediate note overwrite."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_order import source as prior_source


def source():
    text=prior_source()
    hooks={
        '''multi_end3:
    mov a, $32
    bne multi_advance
    mov a, $6a
    beq multi_advance
    jmp pair_reject
''':'''multi_end3:
    mov a, $32
    bne multi_advance
    call reverse_guard
    call poly_all
    call multi_load2
    jmp multi_advance
''',
        '    mov a, $4200+x\n    beq timing_done\ntiming_check3:\n':
            '    mov a, $4200+x\n    beq timing_done\n    call timing_load2\ntiming_check3:\n',
        '''timing_next2:
    mov a, $a2
    and a, #$04
    beq timing_next3
    mov a, $32
    bne timing_next3
    call timing_load2
''':'timing_next2:\n',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('reverse-order hook changed')
        text=text.replace(before,after)
    # The initial following note must overwrite voice 2. A following inactive
    # voice, rest or final stop still needs an independent lifecycle contract.
    return text+'''\n.org $17b0
reverse_guard:
    mov a, $9c
    clrc
    adc a, #$01
    cmp a, $46
    bcs reverse_bad
    mov x, a
    mov a, $4b00+x
    and a, #$04
    beq reverse_bad
    mov a, $30
    clrc
    adc a, #$41
    mov x, a
    mov a, $4200+x
    cmp a, #$c9
    beq reverse_bad
    ret
reverse_bad:
    jmp pair_reject
'''


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('reverse program exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
