#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build channel-2 end priority over a same-tick channel-3 event."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_peer import source as prior_source


def source():
    text=prior_source()
    before='''multi_end2:
    ; Track 2 ends. A peer event at the same tick has unqualified ordering.
    mov a, $33
    bne multi_advance
    mov a, $6b
    beq multi_advance
    jmp pair_reject
'''
    if text.count(before)!=1:raise ValueError('end-priority hook changed')
    return text.replace(before,'multi_end2:\n    ; Channel 2 ends before channel 3 executes at this tick.\n    jmp multi_advance\n')


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('order program exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
