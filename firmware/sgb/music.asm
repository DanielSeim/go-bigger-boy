; SPDX-License-Identifier: GPL-3.0-or-later
; Original fixed-address score interpreter and two eight-note looping motifs.
; Driver calls $0700 each 16-ms timer tick and after committing SOUND.
; $1B pending score: 00 keep, 01/02 restart, 80 stop. $1F enabled.
; $1C countdown, $1D note index, $20 table offset. No borrowed score data.
.org $0700
    mov a, $30
    beq legacy_music
    jmp $1600
legacy_music:
    mov a, $1b
    beq playing
    bmi stop
    cmp a, #$02
    beq score_two
    mov $20, #$00
    bra restart
score_two:
    mov $20, #$08
restart:
    mov $1b, #$00
    mov $1f, #$01
    mov $1c, #$00
    mov $1d, #$00
    mov $f2, #$47
    mov $f3, #$50
playing:
    mov a, $1f
    beq done
    mov a, $1c
    beq note
    dec $1c
    ret
note:
    mov a, $1d
    clrc
    adc a, $20
    mov x, a
    mov $f2, #$5c
    mov $f3, #$10
    mov a, $07d0+x
    mov $f2, #$43
    mov $f3, a
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$10
    mov a, $1d
    clrc
    adc a, #$01
    and a, #$07
    mov $1d, a
    mov $1c, #$0f
    ret
stop:
    mov $1b, #$00
    mov $1f, #$00
    mov $f2, #$47
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$10
done:
    ret
.org $07d0
; Author-defined pitch units, 256-ms steps at the default SPC clock.
.byte $04,$05,$06,$08,$06,$05,$04,$03
.byte $08,$06,$04,$06,$09,$06,$04,$03
