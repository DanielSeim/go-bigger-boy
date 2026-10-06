; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBS1/GBS2 single-track, GBS3 two-track and GBS4/GBS5 finite-phrase subsets.
; Canonical bounded headers/pointers; bank length <=255. No vendor table guesses.
; $30 mode, $31 end, $32 cursor, $33 duration, $34 countdown, $35 held-note flag.
; $36/$37 validation duration/held flag. Validate the entire stream before readiness.
.org $1400
    mov $30, #$00
    mov $4e, #$20
    mov a, $2b00
    cmp a, #$47
    bne magic_error
    mov a, $2b01
    cmp a, #$42
    bne magic_error
    mov a, $2b02
    cmp a, #$53
    bne magic_error
    mov a, $2b03
    cmp a, #$31
    beq format_one
    cmp a, #$35
    beq repeated_phrases
    cmp a, #$34
    beq phrases
    cmp a, #$33
    beq two_tracks
    cmp a, #$32
    bne header_error
    mov $3f, #$02
    bra bank_length
repeated_phrases:
    mov $3f, #$05
    jmp $2200
phrases:
    mov $3f, #$04
    jmp $2200
magic_error:
    jmp header_error
two_tracks:
    jmp $1b00
format_one:
    mov $3f, #$01
bank_length:
    mov a, $2b04
    cmp a, #$23
    bcc header_error
    mov $31, a
    mov a, $2b08
    cmp a, #$10
    bne header_error
    mov a, $2b09
    cmp a, #$2b
    bne header_error
    mov a, $2b10
    cmp a, #$20
    bne header_error
    mov a, $2b11
    cmp a, #$2b
    bne header_error
    mov a, #$05
    mov x, a
reserved:
    cmp x, #$08
    beq skip_phrase
    cmp x, #$10
    beq skip_track
    mov a, $2b00+x
    cmp a, #$00
    bne header_error
    mov a, x
    clrc
    adc a, #$01
    mov x, a
    cmp x, #$20
    bne reserved
    bra validate
skip_phrase:
    mov a, #$0a
    mov x, a
    bra reserved
skip_track:
    mov a, #$12
    mov x, a
    bra reserved
header_error:
    mov a, #$e1
    jmp $0410
syntax_early:
    jmp syntax_error
validate:
    mov $36, #$00
    mov $37, #$00
    mov x, $4e
next:
    mov a, x
    cmp a, $31
    bcs syntax_early
    mov a, $2b00+x
    mov $38, a
    mov a, x
    clrc
    adc a, #$01
    mov x, a
    mov a, $38
    beq terminated
    cmp a, #$80
    bcs event
    mov $36, a
    mov a, x
    cmp a, $31
    bcs syntax_early
    mov a, $2b00+x
    cmp a, #$80
    bcc syntax_early
    bra next
event:
    mov a, $38
    cmp a, #$e0
    beq control
    cmp a, #$e1
    beq control
    cmp a, #$ed
    beq control
    mov a, $36
    beq syntax_error
    mov a, $38
    cmp a, #$c8
    beq tie
    cmp a, #$c9
    beq rest
    cmp a, #$a0
    bcs syntax_error
    mov $37, #$01
    bra next
tie:
    mov a, $37
    beq syntax_error
    bra next
rest:
    mov $37, #$00
    bra next
control:
    jmp $1900
terminated:
    mov a, x
    cmp a, $31
    bne syntax_error
    mov a, $3f
    cmp a, #$03
    bcc validation_done
    mov a, $4d
    bne validation_done
    mov $4d, #$01
    mov $36, #$00
    mov $37, #$00
    mov x, $4b
    mov a, $4c
    mov $31, a
    jmp next
validation_done:
    mov a, $3f
    cmp a, #$04
    bcc publish_format
    jmp $2300
publish_format:
    mov $30, a
    jmp $1000
syntax_error:
    mov a, #$e2
    jmp $0410

.org $1600
    jmp $2000
.org $1700
; Pitch words are generated from 440 Hz tuning and our 16-sample BRR loop.
.org $1740

.org $1800
; Apply one prevalidated uploaded-score control, preserving score time and held note.
    mov x, $32
    mov a, $2b00+x
    mov $39, a
    inc $32
    mov a, $38
    cmp a, #$e0
    beq instrument
    cmp a, #$e1
    beq pan
    mov a, $39
    mov $42, a
    mov a, #$07
    call $1f00
    mov a, $39
    mov $f3, a
    ret
instrument:
    mov a, $39
    mov $40, a
    mov a, #$04
    call $1f00
    mov a, $39
    mov $f3, a
    ret
pan:
    mov a, $39
    mov $41, a
    mov x, a
    mov a, #$00
    call $1f00
    mov a, $1a00+x
    mov $f3, a
    mov a, #$01
    call $1f00
    mov a, $1a15+x
    mov $f3, a
    ret
.org $1880
; Restart restores independent original defaults for all uploaded formats.
    mov $40, #$01
    mov $41, #$0a
    mov $42, #$50
    mov a, #$04
    call $1f00
    mov $f3, #$01
    mov a, #$00
    call $1f00
    mov $f3, #$30
    mov a, #$01
    call $1f00
    mov $f3, #$30
    mov a, #$07
    call $1f00
    mov $f3, #$50
    mov a, #$05
    call $1f00
    mov $f3, #$00
    mov a, #$06
    call $1f00
    mov $f3, #$00
    mov a, $48
    and a, $45
    mov $48, a
    mov $f2, #$5c
    mov $f3, a
    ret

.org $1900
; Check format, complete operand and independent parameter limits before readiness.
    mov a, $3f
    cmp a, #$02
    bcc control_error
    mov a, x
    cmp a, $31
    bcs control_error
    mov a, $2b00+x
    mov $39, a
    mov a, x
    clrc
    adc a, #$01
    mov x, a
    mov a, $38
    cmp a, #$e0
    beq instrument_limit
    cmp a, #$e1
    beq pan_limit
    mov a, $39
    cmp a, #$80
    bcs control_error
    jmp next
instrument_limit:
    mov a, $39
    cmp a, #$04
    bcs control_error
    jmp next
pan_limit:
    mov a, $39
    cmp a, #$15
    bcs control_error
    jmp next
control_error:
    jmp syntax_error
.org $1a00
; Original linear pan law: center is 48/48, endpoints 96/0 and 0/96.
.byte $60,$5b,$56,$52,$4d,$48,$43,$3e,$3a,$35,$30,$2b,$26,$22,$1d,$18,$13,$0e,$0a,$05,$00
.byte $00,$05,$0a,$0e,$13,$18,$1d,$22,$26,$2b,$30,$35,$3a,$3e,$43,$48,$4d,$52,$56,$5b,$60


.org $1b00
; GBS3 fixed two-track header. Both streams are validated before readiness.
    mov $3f, #$03
    mov $4d, #$00
    mov a, $2b04
    cmp a, #$26
    bcc bank_error
    mov $4c, a
    mov a, $2b05
    cmp a, #$23
    bcc bank_error
    cmp a, $4c
    bcs bank_error
    mov $4b, a
    mov $31, a
    mov a, $2b12
    cmp a, $4b
    bne bank_error
    mov a, $2b13
    cmp a, #$2b
    bne bank_error
    mov a, $2b08
    cmp a, #$10
    bne bank_error
    mov a, $2b09
    cmp a, #$2b
    bne bank_error
    mov a, $2b10
    cmp a, #$20
    bne bank_error
    mov a, $2b11
    cmp a, #$2b
    bne bank_error
    mov a, #$06
    mov x, a
bank_reserved:
    cmp x, #$08
    beq bank_skip_phrase
    cmp x, #$10
    beq bank_skip_tracks
    mov a, $2b00+x
    cmp a, #$00
    bne bank_error
    mov a, x
    clrc
    adc a, #$01
    mov x, a
    cmp x, #$20
    bne bank_reserved
    jmp validate
bank_skip_phrase:
    mov a, #$0a
    mov x, a
    bra bank_reserved
bank_skip_tracks:
    mov a, #$14
    mov x, a
    bra bank_reserved
bank_error:
    jmp header_error

.org $1c00
; One physical timer tick advances each track once. Pending command applies to both.
    mov a, $1b
    bne dispatch_command
    mov a, $1f
    beq scheduler_done
    mov $49, #$00
    bra dispatch
scheduler_done:
    ret
dispatch_command:
    mov $49, a
    cmp a, #$01
    bne dispatch
    mov a, $30
    cmp a, #$04
    bcc dispatch
    call $2680
dispatch:
    mov $43, #$40
    mov $44, #$10
    mov $45, #$ef
    mov a, $4e
    mov $4a, a
    call load_track_zero
    mov a, $4b
    mov $31, a
    mov a, $49
    mov $1b, a
    call $2000
    call save_track_zero
    mov $43, #$30
    mov $44, #$08
    mov $45, #$f7
    mov a, $4b
    mov $4a, a
    call load_track_one
    mov a, $4c
    mov $31, a
    mov a, $49
    mov $1b, a
    call $2000
    call save_track_one
    mov a, $58
    clrc
    adc a, $68
    mov $1f, a
    jmp $2700
save_track_zero:
    mov a, $31
    mov $50, a
    mov a, $32
    mov $51, a
    mov a, $33
    mov $52, a
    mov a, $34
    mov $53, a
    mov a, $35
    mov $54, a
    mov a, $40
    mov $55, a
    mov a, $41
    mov $56, a
    mov a, $42
    mov $57, a
    mov a, $1f
    mov $58, a
    ret
load_track_zero:
    mov a, $50
    mov $31, a
    mov a, $51
    mov $32, a
    mov a, $52
    mov $33, a
    mov a, $53
    mov $34, a
    mov a, $54
    mov $35, a
    mov a, $55
    mov $40, a
    mov a, $56
    mov $41, a
    mov a, $57
    mov $42, a
    mov a, $58
    mov $1f, a
    ret
save_track_one:
    mov a, $31
    mov $60, a
    mov a, $32
    mov $61, a
    mov a, $33
    mov $62, a
    mov a, $34
    mov $63, a
    mov a, $35
    mov $64, a
    mov a, $40
    mov $65, a
    mov a, $41
    mov $66, a
    mov a, $42
    mov $67, a
    mov a, $1f
    mov $68, a
    ret
load_track_one:
    mov a, $60
    mov $31, a
    mov a, $61
    mov $32, a
    mov a, $62
    mov $33, a
    mov a, $63
    mov $34, a
    mov a, $64
    mov $35, a
    mov a, $65
    mov $40, a
    mov a, $66
    mov $41, a
    mov a, $67
    mov $42, a
    mov a, $68
    mov $1f, a
    ret

.org $1f00
; Select a voice register: A is offset, $43 is voice base. X stays intact.
    clrc
    adc a, $43
    mov $f2, a
    ret

.org $2000
    mov a, $1b
    beq playing
    bmi early_stop
    mov $1b, #$00
    mov $1f, #$01
    mov a, $4a
    mov $32, a
    mov $33, #$00
    mov $34, #$00
    mov $35, #$00
    call $1880
    bra read
early_stop:
    jmp stop
early_done:
    ret
playing:
    mov a, $1f
    beq early_done
    mov a, $34
    beq read
    dec $34
    mov a, $34
    bne early_done
read:
    mov x, $32
    mov a, x
    cmp a, $31
    bcs early_stop
    mov a, $2b00+x
    mov $38, a
    inc $32
    mov a, $38
    beq early_stop
    cmp a, #$e0
    bcs control_event
    cmp a, #$80
    bcs event_note
    mov $33, a
    bra read
control_event:
    call $1800
    bra read
event_note:
    cmp a, #$c8
    beq duration
    cmp a, #$c9
    beq rest_note
    and a, #$1f
    mov $38, a
    clrc
    adc a, $38
    mov x, a
    mov $f2, #$5c
    mov a, $48
    or a, $44
    mov $f3, a
    mov a, #$02
    call $1f00
    mov a, $1700+x
    mov $f3, a
    mov a, #$03
    call $1f00
    mov a, $1701+x
    mov $f3, a
    mov a, $48
    and a, $45
    mov $48, a
    mov $f2, #$5c
    mov $f3, a
    mov $f2, #$4c
    mov a, $44
    mov $f3, a
    mov $35, #$01
    bra duration
rest_note:
    mov $f2, #$5c
    mov a, $48
    or a, $44
    mov $48, a
    mov $f3, a
    mov $35, #$00
duration:
    mov a, $33
    mov $34, a
done:
    ret
stop:
    mov $1b, #$00
    mov $1f, #$00
    mov $35, #$00
    mov a, #$07
    call $1f00
    mov $f3, #$00
    mov a, $48
    or a, $44
    mov $48, a
    mov $f2, #$5c
    mov $f3, a
    ret

.org $2200
; GBS4/GBS5 finite phrase list: 1..4 canonical two-track pattern tables at $2B20.
    mov a, $2b04
    cmp a, #$36
    bcc phrase_header_error
    mov $77, a
    mov a, $2b05
    beq phrase_header_error
    cmp a, #$05
    bcs phrase_header_error
    mov $70, a
    call $2500
    mov a, $2b07
    bne phrase_header_error
    mov $71, #$00
    mov $72, #$20
    mov $7a, #$08
phrase_words:
    mov x, $7a
    mov a, $2b00+x
    cmp a, $72
    bne phrase_header_error
    mov a, $2b01+x
    cmp a, #$2b
    bne phrase_header_error
    inc $7a
    inc $7a
    mov a, $72
    clrc
    adc a, #$10
    mov $72, a
    inc $71
    mov a, $71
    cmp a, $70
    bne phrase_words
    mov a, $72
    mov $78, a
    mov $79, a
phrase_reserved:
    mov x, $7a
    mov a, $2b00+x
    bne phrase_header_error
    inc $7a
    mov a, $7a
    cmp a, #$20
    bne phrase_reserved
    bra phrase_header_bounds
phrase_header_error:
    jmp header_error
phrase_header_bounds:
    mov a, $79
    clrc
    adc a, #$06
    mov $7c, a
    mov a, $77
    cmp a, $7c
    bcc phrase_header_error
    mov $71, #$00
    mov $72, #$20
    jmp $2400

.org $2300
; Called only after both streams in the current phrase validate completely.
    inc $71
    mov a, $71
    cmp a, $70
    bne validate_another_phrase
    call $2680
    mov a, $3f
    mov $30, a
    jmp $1000
validate_another_phrase:
    mov a, $72
    clrc
    adc a, #$10
    mov $72, a
    jmp $2400

.org $2400
; Canonical contiguous streams; fresh duration and held-note state per track/phrase.
    mov a, $78
    mov $4e, a
    clrc
    adc a, #$03
    bcs phrase_pattern_error
    mov $7c, a
    mov x, $72
    mov a, $2b00+x
    cmp a, $78
    bne phrase_pattern_error
    mov a, $2b01+x
    cmp a, #$2b
    bne phrase_pattern_error
    mov a, $2b02+x
    cmp a, $7c
    bcc phrase_pattern_error
    mov $4b, a
    mov a, $2b03+x
    cmp a, #$2b
    bne phrase_pattern_error
    mov a, $72
    clrc
    adc a, #$10
    mov $7b, a
    mov a, $72
    clrc
    adc a, #$04
    mov $7a, a
pattern_reserved:
    mov x, $7a
    mov a, $2b00+x
    bne phrase_pattern_error
    inc $7a
    mov a, $7a
    cmp a, $7b
    bne pattern_reserved
    mov a, $71
    clrc
    adc a, #$01
    cmp a, $70
    beq final_pattern_end
    mov x, $7b
    mov a, $2b00+x
    bra pattern_end
final_pattern_end:
    mov a, $77
pattern_end:
    mov $4c, a
    cmp a, $77
    bcc pattern_in_bank
    beq pattern_in_bank
    bra phrase_pattern_error
pattern_in_bank:
    mov a, $4b
    clrc
    adc a, #$03
    bcs phrase_pattern_error
    mov $7c, a
    mov a, $4c
    cmp a, $7c
    bcc phrase_pattern_error
    mov $78, a
    mov a, $4b
    mov $31, a
    mov $4d, #$00
    jmp validate
phrase_pattern_error:
    jmp header_error

.org $2500
; GBS4 reserves byte 6; GBS5 defines 1..4 total sequence plays.
    mov $7d, #$01
    mov a, $3f
    cmp a, #$05
    beq repeat_count
    mov a, $2b06
    bne repeat_header_error
    ret
repeat_count:
    mov a, $2b06
    beq repeat_header_error
    cmp a, #$05
    bcs repeat_header_error
    mov $7d, a
    ret
repeat_header_error:
    jmp header_error

.org $2600
; Runtime bounds come only from fully prevalidated canonical pattern pointers.
    mov x, $72
    mov a, $2b00+x
    mov $4e, a
    mov a, $2b02+x
    mov $4b, a
    mov a, $71
    clrc
    adc a, #$01
    cmp a, $70
    beq runtime_final_end
    mov a, $72
    clrc
    adc a, #$10
    mov x, a
    mov a, $2b00+x
    bra runtime_end
runtime_final_end:
    mov a, $77
runtime_end:
    mov $4c, a
    ret
.org $2680
    mov a, $7d
    mov $7e, a
    mov $71, #$00
    mov $72, #$20
    call $2600
    ret

.org $2700
; Barrier: wait for BOTH tracks, then start the next phrase in this same tick.
    mov a, $30
    cmp a, #$04
    bcc phrase_tick_done
    mov a, $1f
    bne phrase_tick_done
    mov a, $49
    bmi phrase_tick_done
    inc $71
    mov a, $71
    cmp a, $70
    beq sequence_finished
    mov a, $72
    clrc
    adc a, #$10
    mov $72, a
    bra start_phrase
sequence_finished:
    dec $7e
    mov a, $7e
    beq phrase_tick_done
    mov $71, #$00
    mov $72, #$20
start_phrase:
    call $2600
    mov $49, #$01
    jmp dispatch
phrase_tick_done:
    ret
