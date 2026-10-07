; SPDX-License-Identifier: GPL-3.0-or-later
; Standalone flat single-track parser appended to the experimental clock.
; Caller: $20 length 1..128, stream at $2B00, tempo at $12.
; Private DP $21..2B; event records at $3000: tick LE16, opcode, duration, articulation.
; Maximum 16 timed events; reject all controls/calls/ties and trailing bytes.
track_start:
    mov a, $20
    beq track_reject
    cmp a, #$81
    bcs track_reject
    call track_clear
track_validate:
    call track_next
    mov a, $2b
    bne track_validate
    call track_clear
    mov $14, #$01
    call track_next
    call track_emit
    ret
track_clear:
    mov $21, #$00
    mov $22, #$00
    mov $23, #$00
    mov $24, #$00
    mov $25, #$00
    mov $27, #$00
    mov $28, #$00
    ret
track_reject:
    mov $f1, #$80
    mov $14, #$e2
track_halted:
    bra track_halted
track_read:
    mov a, $21
    cmp a, $20
    bcs track_reject
    mov x, $21
    mov a, $2b00+x
    inc $21
    cmp a, #$00
    ret
track_next:
    call track_read
    beq track_end
    cmp a, #$80
    bcs track_opcode
    mov $22, a
    call track_read
    cmp a, #$80
    bcs track_opcode
    mov $23, a
    mov $24, #$01
    call track_read
    cmp a, #$80
    bcc track_reject
track_opcode:
    mov $26, a
    cmp a, #$c8
    bcc track_timed
    cmp a, #$c9
    bne track_reject
track_timed:
    mov a, $22
    beq track_reject
    mov $25, a
    mov a, $24
    beq track_reject
    inc $27
    mov a, $27
    cmp a, #$11
    bcs track_reject
    mov $2b, #$01
    ret
track_end:
    mov a, $21
    cmp a, $20
    bne track_reject
    mov a, $27
    beq track_reject
    mov $2b, #$00
    ret
track_tick:
    dec $25
    mov a, $25
    bne track_return
    call track_next
    mov a, $2b
    beq track_finished
    call track_emit
track_return:
    ret
track_finished:
    mov $f1, #$80
    mov $14, #$02
track_complete:
    bra track_complete
track_emit:
    mov x, $28
    mov a, $10
    ; MOV $3000+X,A (absolute indexed stores spelled as explicit opcode bytes).
    .byte $d5, $00, $30
    inc $28
    mov x, $28
    mov a, $11
    .byte $d5, $00, $30
    inc $28
    mov x, $28
    mov a, $26
    .byte $d5, $00, $30
    inc $28
    mov x, $28
    mov a, $22
    .byte $d5, $00, $30
    inc $28
    mov x, $28
    mov a, $23
    .byte $d5, $00, $30
    inc $28
    ret
