; SPDX-License-Identifier: GPL-3.0-or-later
; Both provisional slot-4 short events require full silent profile rehearsal.
short_pair_slot:
    mov a, $47
    cmp a, #$80
    beq short_pair_first
    cmp a, #$82
    bne short_pair_slot_bad
    mov a, $26
    cmp a, #$a1
    beq short_pair_slot_ok
short_pair_slot_bad:
    jmp short_return_gate_bad
short_pair_first:
    mov a, $26
    cmp a, #$a0
    bne short_pair_slot_bad
short_pair_slot_ok:
    jmp short_pair_gate_profile
