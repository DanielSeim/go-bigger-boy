; SPDX-License-Identifier: GPL-3.0-or-later
; $A4 prefix E0 count; $A5 event count; $A6 repetitions; $A7 DSP base;
; $A8/9 queued pre-KON pulses; $AA queue scratch; $AB descriptor field index.
; Parallel $4A00 cache holds instrument selections before each timed event.
reselect_instrument:
    mov a, $4d
    cmp a, #$02
    beq reselect_valid
    jmp pair_reject
reselect_valid:
    inc $a4
    ret
reselect_cache:
    mov x, $47
    mov a, $a4
    .byte $d5, $00, $4a
    mov $a4, #$00
    ret
reselect_event:
    mov x, $63
    mov a, $4a00+x
    mov $a5, a
    mov $a6, a
    mov a, $64
    beq reselect_return
    mov a, $a6
    beq reselect_return
    mov $a7, #$24
    mov a, $23
    cmp a, #$02
    beq reselect_write
    mov $a7, #$34
reselect_write:
    mov $ab, #$00
reselect_fields:
    call reselect_field
    inc $ab
    mov a, $ab
    cmp a, #$04
    bne reselect_fields
    dec $a6
    mov a, $a6
    bne reselect_write
reselect_return:
    ret
reselect_field:
    ; Resolve the independently owned four-field instrument descriptor. Keep
    ; register addressing separate from values and preserve prefix order.
    mov a, $a7
    clrc
    adc a, $ab
    mov $f2, a
    mov x, $ab
    mov a, $1540+x
    mov $f3, a
    ret
reselect_pending:
    ; Timer output includes pulses that occurred during prefix/voice setup,
    ; before this KON. Retain them in the score queue, but do not retroactively
    ; decrement a newly armed voice's gate. $15 includes the current pulse.
    mov a, $fd
    clrc
    adc a, $15
    mov $15, a
    mov $aa, a
    beq reselect_skip_ready
    dec $aa
reselect_skip_ready:
    mov a, $51
    and a, #$04
    beq reselect_skip3
    mov a, $aa
    mov $a8, a
reselect_skip3:
    mov a, $51
    and a, #$08
    beq reselect_return
    mov a, $aa
    mov $a9, a
    ret
reselect_cancel:
    mov a, $70
    and a, #$04
    beq reselect_cancel3
    mov $a8, #$00
reselect_cancel3:
    mov a, $70
    and a, #$08
    beq reselect_return
    mov $a9, #$00
    ret
