; SPDX-License-Identifier: GPL-3.0-or-later
; Muted native finite EF calls: one call per track, nonnested, count 1..3.
; Expanded tracks contain 1..8 timed events in four 32-byte slots at $3100.
; $72 target cursor, $73 return cursor, $74 remaining repetitions,
; $75 nonempty body flag, $76 call-used flag. Duration/articulation persist.
phrase_track:
    mov $4b, #$00
    mov $67, #$00
    mov $68, #$00
    mov $69, #$00
    mov $74, #$00
    mov $75, #$00
    mov $76, #$00
calls_next:
    call phrase_read
    bne calls_nonzero
    jmp calls_end
calls_nonzero:
    cmp a, #$80
    bcs calls_opcode
    mov $68, a
    call phrase_read
    cmp a, #$80
    bcs calls_opcode
    cmp a, #$7f
    beq calls_articulation
    jmp pair_reject
calls_articulation:
    mov $69, #$01
    call phrase_read
calls_opcode:
    mov $26, a
    cmp a, #$ef
    bne calls_not_call
    jmp calls_enter
calls_not_call:
    cmp a, #$80
    bcc calls_bad
    cmp a, #$c8
    bcc calls_timed
    cmp a, #$c9
    beq calls_timed
    ; Setup controls are restricted to the caller before its first timed event.
    mov $4c, a
    mov a, $67
    bne calls_bad
    mov a, $74
    bne calls_bad
    inc $4b
    mov a, $4b
    cmp a, #$06
    bcs calls_bad
    call phrase_control
    jmp calls_next
calls_timed:
    mov a, $68
    beq calls_bad
    mov a, $69
    beq calls_bad
    inc $67
    mov a, $67
    cmp a, #$09
    bcs calls_bad
    mov $75, #$01
    mov a, $68
    call phrase_cache
    mov a, $26
    call phrase_cache
    jmp calls_next
calls_bad:
    jmp pair_reject
calls_enter:
    ; One call site excludes recursive/nested/repeated-call zero-time loops.
    mov a, $76
    bne calls_bad
    mov $76, #$01
    call phrase_word
    call phrase_read
    cmp a, #$01
    bcc calls_bad
    cmp a, #$04
    bcs calls_bad
    mov $74, a
    mov a, $21
    mov $73, a
    call phrase_pointer
    mov a, $21
    mov $72, a
    mov $75, #$00
    jmp calls_next
calls_end:
    mov a, $74
    beq calls_track_end
    mov a, $75
    beq calls_bad
    dec $74
    mov a, $74
    beq calls_return
    mov a, $72
    mov $21, a
    mov $75, #$00
    jmp calls_next
calls_return:
    mov a, $73
    mov $21, a
    jmp calls_next
calls_track_end:
    mov a, $67
    beq calls_bad
    mov a, #$00
    call phrase_cache
    ret
