; SPDX-License-Identifier: GPL-3.0-or-later
; Isolated muted scheduler, NOT an N-SPC bank parser or playback driver.
; $2B00: exactly two patterns, each (duration2, opcode2, duration3, opcode3).
; Durations 1..127, notes $80..C7 or rest $C9. No controls/articulation.
; $30 pattern byte offset, $31 validation cursor, $32/$33 remaining durations.
; $28 log byte count; records at $3000: tick LE16, opcode, duration, channel.
; Both tracks start together; first track end clips its peer and advances pattern.
pair_start:
    mov $28, #$00
    mov a, $20
    cmp a, #$08
    beq pair_validate_start
    jmp pair_reject
pair_validate_start:
    mov $31, #$00
pair_validate:
    mov x, $31
    mov a, $2b00+x
    beq pair_reject
    cmp a, #$80
    bcs pair_reject
    inc $31
    mov x, $31
    mov a, $2b00+x
    cmp a, #$80
    bcc pair_reject
    cmp a, #$c8
    bcc pair_valid_opcode
    cmp a, #$c9
    bne pair_reject
pair_valid_opcode:
    inc $31
    mov a, $31
    cmp a, #$08
    bne pair_validate
    mov $30, #$00
    call pair_pattern
    ret
pair_reject:
    mov $f1, #$80
    mov $14, #$e2
pair_rejected:
    bra pair_rejected
pair_pattern:
    mov $23, #$02
    mov x, $30
    mov a, $2b00+x
    mov $22, a
    mov $32, a
    inc $30
    mov x, $30
    mov a, $2b00+x
    mov $26, a
    inc $30
    call track_emit
    mov $23, #$03
    mov x, $30
    mov a, $2b00+x
    mov $22, a
    mov $33, a
    inc $30
    mov x, $30
    mov a, $2b00+x
    mov $26, a
    inc $30
    call track_emit
    ret
pair_tick:
    dec $32
    dec $33
    mov a, $32
    beq pair_end
    mov a, $33
    beq pair_end
    ret
pair_end:
    mov a, $30
    cmp a, #$08
    beq pair_finished
    call pair_pattern
    ret
pair_finished:
    mov $f1, #$80
    mov $14, #$02
pair_complete:
    bra pair_complete
