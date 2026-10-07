; SPDX-License-Identifier: GPL-3.0-or-later
; Experimental owned waveform, physical voice 2, full-duration gates.
; Notes $98/$99/$A4 only, articulation $7F, duration >= 2; rest $C9.
; Pitch setup uses sanitized previously measured register values, no sample copies.
render_validate:
    mov a, $22
    cmp a, #$02
    bcc render_reject
    mov a, $23
    cmp a, #$7f
    bne render_reject
    mov a, $26
    cmp a, #$98
    beq render_valid
    cmp a, #$99
    beq render_valid
    cmp a, #$a4
    beq render_valid
    cmp a, #$c9
    bne render_reject
render_valid:
    ret
render_reject:
    jmp track_reject
render_setup:
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f2, #$0c
    mov $f3, #$7f
    mov $f2, #$1c
    mov $f3, #$7f
    mov $f2, #$2c
    mov $f3, #$00
    mov $f2, #$3c
    mov $f3, #$00
    mov $f2, #$2d
    mov $f3, #$00
    mov $f2, #$3d
    mov $f3, #$00
    mov $f2, #$4d
    mov $f3, #$00
    mov $f2, #$5d
    mov $f3, #$10
    mov $f2, #$20
    mov $f3, #$50
    mov $f2, #$21
    mov $f3, #$50
    mov $f2, #$24
    mov $f3, #$00
    mov $f2, #$25
    mov $f3, #$00
    mov $f2, #$26
    mov $f3, #$00
    mov $f2, #$27
    mov $f3, #$7f
    mov $f2, #$6c
    mov $f3, #$20
    ret
render_stop:
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$04
    ret
render_event:
    call render_stop
    mov a, $26
    cmp a, #$c9
    beq render_done
    ; Hold key-off for > two DSP frames before a new key-on.
    mov $2c, #$10
render_release_wait:
    dec $2c
    mov a, $2c
    bne render_release_wait
    mov $f2, #$22
    mov a, $26
    cmp a, #$98
    beq render_note24
    cmp a, #$99
    beq render_note25
    mov $f3, #$5c
    mov $f2, #$23
    mov $f3, #$08
    bra render_keyon
render_note24:
    mov $f3, #$2c
    bra render_low_pitch
render_note25:
    mov $f3, #$6c
render_low_pitch:
    mov $f2, #$23
    mov $f3, #$04
render_keyon:
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$04
render_done:
    ret
.org $1000
; Source 0: independently authored square wave, one looping BRR block at $1010.
.byte $10, $10, $10, $10
.org $1010
.byte $b3, $77, $77, $77, $77, $99, $99, $99, $99
