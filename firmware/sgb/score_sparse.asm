; SPDX-License-Identifier: GPL-3.0-or-later
; Owned sparse phrase list: channel 2, channel 3, or both.
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
    bne sparse_pattern_nonzero
    jmp list_terminator
sparse_pattern_nonzero:
    mov a, $46
    cmp a, #$04
    bcc sparse_pattern_bound
    jmp list_parse_bad
sparse_pattern_bound:
    call phrase_pointer
    mov $49, #$00
    mov $a2, #$00
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
    mov a, $60
    xcn a
    mov $47, a
    clrc
    adc a, $47
    mov $47, a
    mov a, $41
    or a, $42
    beq sparse_inactive
    call phrase_pointer
    call phrase_track
    mov a, $49
    cmp a, #$02
    bne sparse_mark3
    mov a, $a2
    or a, #$04
    bra sparse_mark
sparse_mark3:
    mov a, $a2
    or a, #$08
sparse_mark:
    mov $a2, a
    bra sparse_slot_done
sparse_inactive:
    mov x, $47
    mov a, #$00
    .byte $d5, $00, $42
sparse_slot_done:
    inc $60
phrase_next_channel:
    mov a, $4a
    mov $21, a
    mov a, $92
    mov $90, a
    inc $49
    mov a, $49
    cmp a, #$08
    beq sparse_channels_done
    jmp phrase_channel
sparse_channels_done:
    mov a, $a2
    bne sparse_nonempty
    jmp pair_reject
sparse_nonempty:
    mov x, $46
    .byte $d5, $00, $4b
    inc $46
    jmp phrase_pattern
list_terminator:
    mov a, $46
    beq list_parse_bad
    call multi_start
    ret
list_parse_bad:
    jmp pair_reject
