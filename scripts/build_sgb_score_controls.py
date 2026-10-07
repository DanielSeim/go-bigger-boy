#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build isolated native instrument and volume controls with an owned BRR source."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_gate import ROOT, source


def build():
    assembly = source()
    for anchor in ('render_event:\n','gate_validate:\n','.org $1000\n'):
        if assembly.count(anchor)!=1:
            raise ValueError('experimental control source boundary changed')
    start = assembly.index('render_event:\n')
    end = assembly.index('gate_validate:\n',start)
    assembly = assembly[:start] + 'render_event:\n    jmp controls_render_event\n' + assembly[end:]
    hooks = {
        '    cmp a, #$80\n    beq clock_start\n    cmp a, #$c0\n    beq clock_start\n': '',
        'track_clear:\n': 'track_clear:\n    mov $30, #$02\n    mov $31, #$a0\n    mov $32, #$7f\n',
        'track_opcode:\n    mov $26, a\n':
            'track_opcode:\n    mov $26, a\n    cmp a, #$e0\n    bcc controls_timed_dispatch\n'
            '    call controls_command\n    jmp track_next\ncontrols_timed_dispatch:\n',
        '    call gate_validate\n': '    call controls_validate\n',
        '.org $0e00\n': (ROOT/'firmware/sgb/score_controls.asm').read_text() + '\n.org $0e00\n',
    }
    for old,new in hooks.items():
        if assembly.count(old)!=1:
            raise ValueError('experimental instrument/volume hook changed')
        assembly=assembly.replace(old,new)
    directory = bytes(44)
    directory = bytearray(directory)
    for slot in (2,10):
        directory[slot*4:slot*4+4]=bytes((0,0x11,0,0x11))
    start=assembly.index('.org $1000\n')
    assembly=assembly[:start]+'.org $1000\n.byte '+', '.join(f'${value:02x}' for value in directory)+(
        '\n.org $1100\n.byte $b3, $77, $77, $77, $77, $99, $99, $99, $99\n')
    image=bytes(assemble(assembly,'spc',0x0800))
    if len(image)>4096:
        raise ValueError('experimental control image exceeds code/source bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(f'Experimental score controls: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':
    main()
