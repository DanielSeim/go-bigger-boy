#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build variable bounded phrase lists with eight track slots and a 16-bit log."""
import argparse
import hashlib
from pathlib import Path
import re
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT, source as bank_source


def source():
    text = bank_source()
    first, last = 'phrase_begin:\n', 'phrase_read:\n'
    if text.count(first) != 1 or text.count(last) != 1 or text.index(first) >= text.index(last):
        raise ValueError('experimental phrase-list parser boundary changed')
    text = text[:text.index(first)]+(ROOT/'firmware/sgb/score_list.asm').read_text()+'\n'+text[text.index(last):]
    # Reserve both $4000/$4100 log pages, move parallel caches one page up.
    text = re.sub(r'\$4[1-8][0-9a-f]{2}\b', lambda m:f'${int(m[0][1:],16)+0x100:04x}',text)
    text = re.sub(r'(\.byte \$d5, \$00, )\$(4[1-8])\b',lambda m:m[1]+f'${int(m[2],16)+1:02x}',text)
    hooks = {
        '    mov $28, #$00\n': '    mov $28, #$00\n    mov $9b, #$00\n    mov $a1, #$00\n',
        'multi_start:\n': 'multi_start:\n    mov $9d, #$00\n    mov $9e, #$00\n',
        'multi_begin:\n': 'multi_begin:\n    mov $9c, #$00\n',
        'pair_tick:\n': 'pair_tick:\n    call list_tick_budget\n',
        'multi_emit_ready:\n': 'multi_emit_ready:\n    mov $a1, #$01\n',
        '    call gates_event\n    call mix_voice\n': '    mov $a1, #$00\n    call gates_event\n    call mix_voice\n',
        '    .byte $d5, $00, $40\n    inc $28\n': '    call list_log_store\n',
    }
    for old,new in hooks.items():
        count = 5 if '.byte $d5' in old else 2 if 'mov $28, #$00' in old else 1
        if text.count(old) != count:
            raise ValueError('experimental phrase-list runtime hook changed')
        text = text.replace(old,new)
    first,last='multi_advance:\n','multi_finished:\n'
    if text.count(first)!=1 or text.count(last)!=1:
        raise ValueError('experimental phrase-list transition boundary changed')
    replacement='''multi_advance:
    inc $9c
    mov a, $9c
    cmp a, $46
    beq multi_finished
    mov a, $30
    clrc
    adc a, #$40
    mov $30, a
    call multi_pattern
    ret
'''
    text=text[:text.index(first)]+replacement+text[text.index(last):]
    return text+'\n.org $1400\n'+(ROOT/'firmware/sgb/score_list_runtime.asm').read_text()


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:
        raise ValueError('experimental phrase-list image exceeds code/source bound')
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
    print(f'Experimental phrase list: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':
    main()
