; SPDX-License-Identifier: GPL-3.0-or-later
; Explicit calibrated profiles; no interpolation or general articulation law.
; $2D gate countdown, $2E selected pulse count, $2F bounded table cursor.
; All timed events retain score duration; only notes start gates.
gate_validate:
    mov a, $23
    cmp a, #$7f
    beq gate_note_check
    cmp a, #$3f
    bne gate_reject
gate_note_check:
    mov a, $26
    cmp a, #$c9
    beq gate_rest_check
    cmp a, #$98
    beq gate_find
    cmp a, #$99
    beq gate_find
    cmp a, #$a4
    bne gate_reject
gate_find:
    mov $2f, #$00
gate_lookup:
    mov x, $2f
    mov a, $0e00+x
    cmp a, $12
    bne gate_next_profile
    mov a, $0e01+x
    cmp a, $23
    bne gate_next_profile
    mov a, $0e02+x
    cmp a, $22
    bne gate_next_profile
    mov a, $0e03+x
    mov $2e, a
    ret
gate_next_profile:
    mov a, $2f
    clrc
    adc a, #$04
    mov $2f, a
    cmp a, #$28
    bcc gate_lookup
    bra gate_reject
gate_rest_check:
    mov a, $22
    cmp a, #$02
    bcc gate_reject
    mov $2e, #$00
    ret
gate_reject:
    jmp track_reject
gate_start:
    mov a, $2e
    mov $2d, a
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
.org $0e00
; tempo, articulation, duration, timer pulses; fitted to controlled observations.
.byte $60, $7f, $08, $0f
.byte $60, $3f, $08, $0a
.byte $60, $7f, $10, $24
.byte $60, $3f, $10, $17
.byte $60, $7f, $18, $3a
.byte $60, $3f, $18, $25
.byte $80, $7f, $10, $1b
.byte $80, $3f, $10, $11
.byte $c0, $7f, $10, $12
.byte $c0, $3f, $10, $0b
