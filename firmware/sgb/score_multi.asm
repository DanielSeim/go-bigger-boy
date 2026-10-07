; SPDX-License-Identifier: GPL-3.0-or-later
; Muted two-pattern scheduler: 1..4 events per track, duration/articulation inheritance.
; Four fixed 16-byte native cache slots at $3100; (duration, opcode) pairs then zero.
; $60 parsed-track slot, $61/62 live cursors, $63 load cursor, $64 replay flag,
; $66 validation complete, $67 event count, $68 duration, $69 articulation present,
; $6A/6B pending durations. No DSP rendering or mailbox publication.
phrase_track:
    mov $4b, #$00
    mov $67, #$00
    mov $68, #$00
    mov $69, #$00
multi_prefix:
    call phrase_read
    beq multi_parse_bad
    cmp a, #$80
    bcc multi_duration
    mov $4c, a
    inc $4b
    mov a, $4b
    cmp a, #$06
    bcs multi_parse_bad
    call phrase_control
    bra multi_prefix
multi_next:
    call phrase_read
    beq multi_track_end
    cmp a, #$80
    bcs multi_opcode
multi_duration:
    mov $68, a
    call phrase_read
    cmp a, #$80
    bcs multi_opcode
    cmp a, #$7f
    bne multi_parse_bad
    mov $69, #$01
    call phrase_read
multi_opcode:
    mov $26, a
    cmp a, #$80
    bcc multi_parse_bad
    cmp a, #$c8
    bcc multi_timed
    cmp a, #$c9
    bne multi_parse_bad
multi_timed:
    mov a, $68
    beq multi_parse_bad
    mov a, $69
    beq multi_parse_bad
    inc $67
    mov a, $67
    cmp a, #$05
    bcs multi_parse_bad
    mov a, $68
    call phrase_cache
    mov a, $26
    call phrase_cache
    bra multi_next
multi_track_end:
    mov a, $67
    beq multi_parse_bad
    mov a, #$00
    call phrase_cache
    ret
multi_parse_bad:
    jmp pair_reject
multi_start:
    ; Run the same countdowns silently before publishing any real events.
    ; This bounded rehearsal uses no timer pulses or public score-tick increments.
    mov $64, #$00
    mov $66, #$00
    call multi_begin
multi_rehearse:
    call pair_tick
    mov a, $66
    beq multi_rehearse
    mov $64, #$01
    call multi_begin
    ret
multi_begin:
    mov $28, #$00
    mov $30, #$00
    call multi_pattern
    ret
multi_pattern:
    mov a, $30
    mov $61, a
    clrc
    adc a, #$10
    mov $62, a
    call multi_load2
    call multi_load3
    ret
multi_load2:
    mov $23, #$02
    mov a, $61
    mov $63, a
    call multi_event
    mov a, $63
    mov $61, a
    mov a, $22
    mov $32, a
    ret
multi_load3:
    mov $23, #$03
    mov a, $62
    mov $63, a
    call multi_event
    mov a, $63
    mov $62, a
    mov a, $22
    mov $33, a
    ret
multi_event:
    mov x, $63
    mov a, $3100+x
    mov $22, a
    inc $63
    mov x, $63
    mov a, $3100+x
    mov $26, a
    inc $63
    call track_emit
    ret
pair_tick:
    dec $32
    dec $33
    mov $6a, #$01
    mov $6b, #$01
    mov a, $32
    bne multi_check3
    mov x, $61
    mov a, $3100+x
    mov $6a, a
multi_check3:
    mov a, $33
    bne multi_end_check
    mov x, $62
    mov a, $3100+x
    mov $6b, a
multi_end_check:
    mov a, $6a
    beq multi_end2
    mov a, $6b
    beq multi_end3
    mov a, $32
    bne multi_emit3
    call multi_load2
multi_emit3:
    mov a, $33
    bne multi_tick_done
    call multi_load3
multi_tick_done:
    ret
multi_end2:
    ; Track 2 ends. A peer event at the same tick has unqualified ordering.
    mov a, $33
    bne multi_advance
    mov a, $6b
    beq multi_advance
    jmp pair_reject
multi_end3:
    mov a, $32
    bne multi_advance
    mov a, $6a
    beq multi_advance
    jmp pair_reject
multi_advance:
    mov a, $30
    cmp a, #$20
    beq multi_finished
    mov $30, #$20
    call multi_pattern
    ret
multi_finished:
    mov a, $64
    bne multi_complete
    mov $66, #$01
    ret
multi_complete:
    mov $f1, #$80
    mov $14, #$02
multi_halted:
    bra multi_halted
