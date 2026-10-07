; SPDX-License-Identifier: GPL-3.0-or-later
; Measured instrument-2 mix points; diagnostic source/envelope remain owned.
; $81/82 parser pan/track, $83 global song, $84 song-set flag,
; $85/86 selected L/R, $87 bounded control count, $88 DSP address scratch,
; $89..8D event L/R/pan/track/song. Parallel event caches $3400..3800.
mix_init:
    mov $83, #$a0
    mov $84, #$00
    ret
mix_track:
    mov $81, #$0a
    mov $82, #$7f
    mov $87, #$00
    ret
mix_command:
    inc $87
    mov a, $87
    cmp a, #$21
    bcc mix_read
    jmp pair_reject
mix_read:
    call phrase_read
    mov $4d, a
    mov a, $4c
    cmp a, #$e1
    beq mix_pan
    cmp a, #$ed
    beq mix_track_volume
    cmp a, #$e5
    beq mix_song
    cmp a, #$e0
    beq mix_instrument
    cmp a, #$e7
    beq mix_tempo
mix_bad:
    jmp pair_reject
mix_pan:
    mov a, $4d
    cmp a, #$00
    beq mix_pan_ok
    cmp a, #$0a
    beq mix_pan_ok
    cmp a, #$14
    bne mix_bad
mix_pan_ok:
    mov $81, a
    ret
mix_track_volume:
    mov a, $4d
    cmp a, #$7f
    beq mix_track_ok
    cmp a, #$40
    bne mix_bad
mix_track_ok:
    mov $82, a
    ret
mix_song:
    ; Global song volume can be selected once, in the first channel-2 prefix.
    mov a, $60
    bne mix_bad
    mov a, $84
    bne mix_bad
    mov a, $4d
    cmp a, #$a0
    beq mix_song_ok
    cmp a, #$50
    bne mix_bad
mix_song_ok:
    mov $83, a
    mov $84, #$01
    ret
mix_instrument:
    mov a, $4d
    cmp a, #$02
    bne mix_bad
    ret
mix_tempo:
    mov a, $4d
    cmp a, $12
    bne mix_bad
    ret
mix_validate:
    mov $85, #$00
    mov $86, #$00
    mov a, $26
    cmp a, #$c9
    beq mix_done
    mov a, $81
    cmp a, #$0a
    beq mix_center
    mov a, $83
    cmp a, #$a0
    bne mix_bad
    mov a, $82
    cmp a, #$7f
    bne mix_bad
    mov a, $81
    beq mix_right
    mov $85, #$0b
    ret
mix_right:
    mov $86, #$0b
    ret
mix_center:
    mov a, $83
    cmp a, #$a0
    beq mix_center_track
    mov a, $82
    cmp a, #$7f
    bne mix_bad
    bra mix_reduced
mix_center_track:
    mov a, $82
    cmp a, #$7f
    bne mix_reduced
    mov $85, #$07
    mov $86, #$07
    ret
mix_reduced:
    mov $85, #$01
    mov $86, #$01
mix_done:
    ret
mix_cache:
    mov x, $47
    mov a, $85
    .byte $d5, $00, $34
    mov a, $86
    .byte $d5, $00, $35
    mov a, $81
    .byte $d5, $00, $36
    mov a, $82
    .byte $d5, $00, $37
    mov a, $83
    .byte $d5, $00, $38
    ret
mix_event:
    mov x, $63
    mov a, $33fe+x
    mov $89, a
    mov a, $34fe+x
    mov $8a, a
    mov a, $35fe+x
    mov $8b, a
    mov a, $36fe+x
    mov $8c, a
    mov a, $37fe+x
    mov $8d, a
    ret
mix_voice:
    mov a, $26
    cmp a, #$c9
    beq mix_done
    mov a, $23
    cmp a, #$02
    bne mix_voice3
    mov $88, #$20
    bra mix_write
mix_voice3:
    mov $88, #$30
mix_write:
    mov a, $88
    mov $f2, a
    mov a, $89
    mov $f3, a
    mov a, $88
    clrc
    adc a, #$01
    mov $f2, a
    mov a, $8a
    mov $f3, a
    ret
