; SPDX-License-Identifier: GPL-3.0-or-later
; Owned 1..4-pattern phrase list; source cursors remain bounded 16-bit.
; $46 validated pattern count, $60 expanded track slot 0..7.
phrase_begin:
    mov $21, #$00
    mov $90, #$2b
    mov $46, #$00
    mov $60, #$00
    call phrase_word
    call phrase_pointer
    mov a, $21
    mov $43, a
    mov a, $90
    mov $91, a
phrase_pattern:
    mov a, $43
    mov $21, a
    mov a, $91
    mov $90, a
    call phrase_word
    mov a, $21
    mov $43, a
    mov a, $90
    mov $91, a
    mov a, $41
    or a, $42
    beq list_terminator
    mov a, $46
    cmp a, #$04
    bcs list_parse_bad
    call phrase_pointer
    mov $49, #$00
phrase_channel:
    call phrase_word
    mov a, $21
    mov $4a, a
    mov a, $90
    mov $92, a
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
    mov a, $60
    xcn a
    mov $47, a
    clrc
    adc a, $47
    mov $47, a
    call phrase_track
    inc $60
phrase_next_channel:
    mov a, $4a
    mov $21, a
    mov a, $92
    mov $90, a
    inc $49
    mov a, $49
    cmp a, #$08
    bne phrase_channel
    inc $46
    jmp phrase_pattern
list_terminator:
    mov a, $46
    beq list_parse_bad
    call multi_start
    ret
list_parse_bad:
    jmp pair_reject
