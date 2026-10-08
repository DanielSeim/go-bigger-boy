; SPDX-License-Identifier: GPL-3.0-or-later
; Provisional short gate only in slot 4's first record. Rehearsal must qualify it.
short_return_gate:
    mov a, $26
    cmp a, #$c9
    beq short_return_gate_old
    mov a, $68
    cmp a, #$04
    bne short_return_gate_old
    mov a, $60
    cmp a, #$04
    bne short_return_gate_bad
    mov a, $47
    cmp a, #$80
    bne short_return_gate_bad
    mov a, $26
    cmp a, #$a0
    bne short_return_gate_bad
    mov a, $12
    cmp a, #$60
    bne short_return_gate_bad
    mov a, $69
    cmp a, #$3f
    beq short_return_gate_ok
    cmp a, #$7f
    bne short_return_gate_bad
short_return_gate_ok:
    mov $c4, #$01
    mov $76, #$05
    ret
short_return_gate_bad:
    jmp pair_reject
short_return_gate_old:
    jmp gates_validate
