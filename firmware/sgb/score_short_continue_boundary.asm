; SPDX-License-Identifier: GPL-3.0-or-later
; Only the measured short direct return may extend both tracks at tick 68.
short_continue_boundary:
    mov a, $42a2
    beq short_continue_boundary_old
    mov a, $c4
    beq short_continue_boundary_bad
    mov a, $4282
    mov $bf, a
    mov a, $42a2
    cmp a, $bf
    bne short_continue_boundary_bad
    mov a, $42a3
    cmp a, #$c9
    bne short_continue_boundary_bad
    mov a, $42a4
    bne short_continue_boundary_bad
    mov a, $4480
    mov $bf, a
    mov a, $44a2
    cmp a, $bf
    bne short_continue_boundary_bad
    mov $c5, #$01
short_continue_boundary_old:
    mov a, #$01
    ret
short_continue_boundary_bad:
    mov a, #$00
    ret
