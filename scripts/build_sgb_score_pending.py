#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build pending channel-2 KON and measured returning-rest release diagnostics."""
import argparse,hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_reverse import source as prior_source


def source():
    text=prior_source()
    hooks={
        'multi_begin:\n':'multi_begin:\n    mov $b5, #$00\n    mov $b6, #$00\n    mov $b7, #$00\n',
        '    call reverse_guard\n    call poly_all\n    call multi_load2\n':
            '    call reverse_guard\n    call poly_all\n    call multi_load2\n    call pending_boundary\n',
        'multi_pattern:\n    call poly_all\n':'multi_pattern:\n    call pending_pattern\n',
        '    mov $70, a\n    mov $32, #$01\n':'    or a, $b6\n    mov $70, a\n    mov $32, #$01\n',
        'sparse_start_on:\n    call poly_on\n':'sparse_start_on:\n    call poly_on\n    mov $b6, #$00\n',
        '    call inherit_event\n    call track_emit\n':'    call inherit_event\n    call pending_event\n    call track_emit\n',
        '    call mix_voice\n    call duet_voice\n':'    call pending_mix\n    call duet_voice\n',
        '    and a, #$04\n    beq reverse_bad\n':'    and a, #$04\n    beq pending_guard_jump\n',
        'reverse_bad:\n': 'pending_guard_jump:\n    jmp pending_guard\nreverse_bad:\n',
        '.org $0e00':'\n'+'''pending_pattern:
    call poly_all
    mov a, $b6
    mov $51, a
    ret
pending_boundary:
    mov a, $b5
    beq pending_return
    mov $b5, #$00
    mov a, $a5
    bne pending_bad
    mov a, $22
    cmp a, #$08
    beq pending_duration_ok
    cmp a, #$10
    bne pending_bad
pending_duration_ok:
    mov a, $26
    cmp a, #$c9
    beq pending_return
    mov $b6, #$04
    mov $b7, #$01
pending_return:
    ret
pending_mix:
    mov a, $b5
    bne pending_return
    jmp mix_voice
pending_event:
    mov a, $b7
    beq pending_return
    mov a, $23
    cmp a, #$02
    bne pending_return
    mov a, $a5
    bne pending_bad
    mov $b7, #$00
    mov a, $26
    cmp a, #$c9
    bne pending_return
    ; Lookup a measured duration/articulation profile without generating a
    ; note or changing the rest record. Rehearsal rejects unknown profiles.
    mov a, $22
    cmp a, #$08
    beq pending_rest_ok
    cmp a, #$10
    bne pending_bad
pending_rest_ok:
    mov $26, #$98
    mov a, $22
    mov $77, a
    mov a, $7b
    mov $69, a
    call gates_lookup
    mov $26, #$c9
    mov a, $76
    mov $72, a
    ret
pending_bad:
    jmp pair_reject
.org $0e00''',
        '.org $1300':'\n'+'''pending_guard:
    mov a, $12
    cmp a, #$60
    bne pending_guard_bad
    ; Only one inactive pattern followed by channel 2 is qualified here.
    mov a, $9c
    clrc
    adc a, #$02
    cmp a, $46
    bcs pending_guard_bad
    mov x, a
    mov a, $4b00+x
    cmp a, #$04
    bne pending_guard_bad
    mov $b5, #$01
    ret
pending_guard_bad:
    jmp pair_reject
.org $1300''',
    }
    for before,after in hooks.items():
        if text.count(before)!=1:raise ValueError('pending lifecycle hook changed: '+before)
        text=text.replace(before,after)
    return text


def build():
    image=bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096:raise ValueError('pending program exceeds diagnostic bound')
    return image


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    try:
        with a.output.open('xb') as out:out.write(build())
    except (OSError,ValueError) as error:p.error(str(error))
    print(hashlib.sha256(build()).hexdigest())
