; SPDX-License-Identifier: GPL-3.0-or-later
; Isolated muted native N-SPC subset: bank $2B00, length $20 (1..128).
; Root word at bank offset 0 -> exactly two pattern words and a zero word.
; Pattern tables have eight channel words; only channels 2 and 3 are active.
; Each stream: <=5 bounded setup commands, duration, articulation 127, note/rest, 0.
; Native prevalidation builds eight duration/opcode bytes at $3100 for pair runtime.
; No host-generated schedule; no DSP renderer or mailbox publication.
; Private DP $21 read cursor, $41/42 word, $43 phrase cursor, $46 pattern count,
; $47 cache cursor, $49 channel, $4A saved table cursor, $4B control count,
; $4C control opcode, $4D operand. These do not overlap pair countdowns or clock.
pair_start:
    mov $28, #$00
    mov a, $20
    beq phrase_start_bad
    cmp a, #$81
    bcs phrase_start_bad
    jmp phrase_begin
phrase_start_bad:
    jmp pair_reject
phrase_begin:
    mov $21, #$00
    mov $46, #$00
    mov $47, #$00
    call phrase_word
    call phrase_pointer
    mov a, $21
    mov $43, a
phrase_pattern:
    mov a, $43
    mov $21, a
    call phrase_word
    mov a, $21
    mov $43, a
    call phrase_pointer
    mov $49, #$00
phrase_channel:
    call phrase_word
    mov a, $21
    mov $4a, a
    mov a, $49
    cmp a, #$02
    beq phrase_active
    cmp a, #$03
    beq phrase_active
    mov a, $41
    or a, $42
    beq phrase_next_channel
    jmp pair_reject
phrase_active:
    call phrase_pointer
    call phrase_track
phrase_next_channel:
    mov a, $4a
    mov $21, a
    inc $49
    mov a, $49
    cmp a, #$08
    bne phrase_channel
    inc $46
    mov a, $46
    cmp a, #$02
    beq phrase_terminator
    jmp phrase_pattern
phrase_terminator:
    mov a, $43
    mov $21, a
    call phrase_word
    mov a, $41
    or a, $42
    beq phrase_valid
    jmp pair_reject
phrase_valid:
    mov $30, #$00
    call pair_pattern
    ret
phrase_read:
    mov a, $21
    cmp a, $20
    bcc phrase_read_ok
    jmp pair_reject
phrase_read_ok:
    mov x, $21
    mov a, $2b00+x
    inc $21
    cmp a, #$00
    ret
phrase_word:
    call phrase_read
    mov $41, a
    call phrase_read
    mov $42, a
    ret
phrase_pointer:
    mov a, $42
    cmp a, #$2b
    bne phrase_pointer_bad
    mov a, $41
    cmp a, $20
    bcs phrase_pointer_bad
    mov $21, a
    ret
phrase_pointer_bad:
    jmp pair_reject
phrase_track:
    mov $4b, #$00
phrase_track_next:
    call phrase_read
    beq phrase_track_bad
    cmp a, #$80
    bcc phrase_duration
    mov $4c, a
    inc $4b
    mov a, $4b
    cmp a, #$06
    bcs phrase_track_bad
    call phrase_control
    bra phrase_track_next
phrase_duration:
    call phrase_cache
    call phrase_read
    cmp a, #$7f
    bne phrase_track_bad
    call phrase_read
    cmp a, #$80
    bcc phrase_track_bad
    cmp a, #$c8
    bcc phrase_note
    cmp a, #$c9
    bne phrase_track_bad
phrase_note:
    call phrase_cache
    call phrase_read
    bne phrase_track_bad
    ret
phrase_track_bad:
    jmp pair_reject
phrase_cache:
    mov x, $47
    ; MOV $3100+X,A. Four validated tracks produce exactly eight bytes.
    .byte $d5, $00, $31
    inc $47
    ret
phrase_control:
    call phrase_read
    mov $4d, a
    mov a, $4c
    cmp a, #$e0
    beq phrase_instrument
    cmp a, #$e1
    beq phrase_pan
    cmp a, #$ed
    beq phrase_volume
    cmp a, #$e5
    beq phrase_song_volume
    cmp a, #$e7
    beq phrase_tempo
    jmp pair_reject
phrase_instrument:
    mov a, $4d
    cmp a, #$02
    beq phrase_control_ok
    jmp pair_reject
phrase_pan:
    mov a, $4d
    cmp a, #$0a
    beq phrase_control_ok
    jmp pair_reject
phrase_volume:
    mov a, $4d
    cmp a, #$7f
    beq phrase_control_ok
    jmp pair_reject
phrase_song_volume:
    mov a, $4d
    cmp a, #$a0
    beq phrase_control_ok
    jmp pair_reject
phrase_tempo:
    mov a, $4d
    cmp a, $12
    beq phrase_control_ok
    jmp pair_reject
phrase_control_ok:
    ret
