; SPDX-License-Identifier: GPL-3.0-or-later
; Independent measured articulation-127 gates. No interpolation/general law.
; $72/73 active pulse counters, $74/75 lookup cursor/expired mask,
; $76 selected pulses, $77 duration, $78/79 pending pulses, $7A release cause.
; Cause 0 scheduler/transition; 1 timer gate. Preserve the unaffected held KOF bit.
gates_init:
    mov $72, #$00
    mov $73, #$00
    mov $78, #$00
    mov $79, #$00
    mov $7a, #$00
    ret
gates_validate:
    mov a, $68
    mov $77, a
    call gates_lookup
    ret
gates_lookup:
    mov $76, #$00
    mov a, $26
    cmp a, #$c9
    beq gates_return
    mov $74, #$00
gates_find:
    mov x, $74
    mov a, $0f80+x
    cmp a, $12
    bne gates_next
    mov a, $0f81+x
    cmp a, $77
    bne gates_next
    mov a, $0f82+x
    mov $76, a
    ret
gates_next:
    mov a, $74
    clrc
    adc a, #$03
    mov $74, a
    cmp a, #$0f
    bcc gates_find
    jmp pair_reject
gates_return:
    ret
gates_cache:
    mov x, $47
    mov a, $76
    ; Parallel native cache: pulse count at $3200 + duration/opcode pair offset.
    .byte $d5, $00, $32
    ret
gates_event:
    mov x, $63
    mov a, $31fe+x
    mov $76, a
    mov a, $23
    cmp a, #$02
    bne gates_pending3
    mov a, $76
    mov $78, a
    ret
gates_pending3:
    mov a, $76
    mov $79, a
    ret
gates_on:
    mov a, $51
    and a, #$04
    beq gates_on3
    mov a, $78
    mov $72, a
gates_on3:
    mov a, $51
    and a, #$08
    beq gates_return
    mov a, $79
    mov $73, a
    ret
gates_cancel:
    mov $7a, #$00
    mov a, $70
    and a, #$04
    beq gates_cancel3
    mov $72, #$00
    mov $78, #$00
gates_cancel3:
    mov a, $70
    and a, #$08
    beq gates_return
    mov $73, #$00
    mov $79, #$00
    ret
gates_pulse:
    mov $75, #$00
    mov a, $72
    beq gates_pulse3
    dec $72
    mov a, $72
    bne gates_pulse3
    mov $75, #$04
gates_pulse3:
    mov a, $73
    beq gates_expire
    dec $73
    mov a, $73
    bne gates_expire
    mov a, $75
    or a, #$08
    mov $75, a
gates_expire:
    mov a, $75
    beq gates_return
    mov $7a, #$01
    mov $70, a
    mov $f2, #$4c
    mov $f3, #$00
    mov a, $71
    or a, $75
    mov $71, a
    mov $f2, #$5c
    mov $f3, a
    ret
