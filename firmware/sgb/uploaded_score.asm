; SPDX-License-Identifier: GPL-3.0-or-later
; Independently written GBS1 single-channel subset. No vendor song-table guesses.
; Fixed header/phrase/pattern locations; length <=255, track begins at $2B20.
; $30 mode, $31 end, $32 cursor, $33 duration, $34 countdown, $35 held-note flag.
; $36/$37 validation duration/held flag. Validate the entire stream before readiness.
.org $1400
    mov $30, #$00
    mov a, $2b00
    cmp a, #$47
    bne header_error
    mov a, $2b01
    cmp a, #$42
    bne header_error
    mov a, $2b02
    cmp a, #$53
    bne header_error
    mov a, $2b03
    cmp a, #$31
    bne header_error
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
validate:
    mov $36, #$00
    mov $37, #$00
    mov a, #$20
    mov x, a
next:
    mov a, x
    cmp a, $31
    bcs syntax_error
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
    bcs syntax_error
    mov a, $2b00+x
    cmp a, #$80
    bcc syntax_error
    bra next
event:
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
terminated:
    mov a, x
    cmp a, $31
    bne syntax_error
    mov $30, #$01
    jmp $1000
syntax_error:
    mov a, #$e2
    jmp $0410

.org $1600
    mov a, $1b
    beq playing
    bmi early_stop
    mov $1b, #$00
    mov $1f, #$01
    mov $32, #$20
    mov $33, #$00
    mov $34, #$00
    mov $35, #$00
    mov $f2, #$47
    mov $f3, #$50
    bra read
early_stop:
    jmp stop
playing:
    mov a, $1f
    beq done
    mov a, $34
    beq read
    dec $34
    mov a, $34
    bne done
read:
    mov x, $32
    mov a, x
    cmp a, $31
    bcs stop
    mov a, $2b00+x
    mov $38, a
    inc $32
    mov a, $38
    beq stop
    cmp a, #$80
    bcs event_note
    mov $33, a
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
    mov $f3, #$10
    mov a, $1700+x
    mov $f2, #$42
    mov $f3, a
    mov a, $1701+x
    mov $f2, #$43
    mov $f3, a
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$10
    mov $35, #$01
    bra duration
rest_note:
    mov $f2, #$5c
    mov $f3, #$10
    mov $35, #$00
duration:
    mov a, $33
    mov $34, a
done:
    ret
stop:
    mov $1b, #$00
    mov $1f, #$00
    mov $f2, #$47
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$10
    ret
.org $1700
; Pitch words are generated from 440 Hz tuning and our 16-sample BRR loop.
