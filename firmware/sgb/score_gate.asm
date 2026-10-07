; SPDX-License-Identifier: GPL-3.0-or-later
; Independently specified calibrated duration-16 gate profiles, not a general law.
; $2D timer-pulse countdown: (tempo96, art7F)=36; (192,7F)=18; (96,3F)=23.
; All timed events retain the normal score duration; only notes start gates.
gate_validate:
    mov a, $23
    cmp a, #$7f
    beq gate_note_check
    cmp a, #$3f
    bne gate_reject
    mov a, $12
    cmp a, #$60
    bne gate_reject
gate_note_check:
    mov a, $26
    cmp a, #$c9
    beq gate_rest_check
    mov a, $22
    cmp a, #$10
    bne gate_reject
    mov a, $26
    cmp a, #$98
    beq gate_valid
    cmp a, #$99
    beq gate_valid
    cmp a, #$a4
    bne gate_reject
gate_valid:
    ret
gate_rest_check:
    mov a, $22
    cmp a, #$02
    bcc gate_reject
    ret
gate_reject:
    jmp track_reject
gate_start:
    mov a, $23
    cmp a, #$3f
    beq gate_short
    mov a, $12
    cmp a, #$c0
    beq gate_fast
    mov $2d, #$24
    ret
gate_short:
    mov $2d, #$17
    ret
gate_fast:
    mov $2d, #$12
    ret
gate_pulse:
    mov a, $2d
    beq gate_return
    dec $2d
    mov a, $2d
    bne gate_return
    call render_stop
gate_return:
    ret
