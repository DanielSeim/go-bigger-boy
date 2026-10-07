; SPDX-License-Identifier: GPL-3.0-or-later
; Bounded E0/E1/E5/ED controls. Private $30 instrument, $31 song, $32 track, $33 pan, $34 scratch.
; Instrument 2: full pair (160,127), reduced song (80,127), reduced track (160,64).
; Instrument 10: full pair only. Native notes require tempo96/duration16/art7F.
controls_command:
    mov a, $26
    cmp a, #$e0
    beq controls_instrument
    cmp a, #$e1
    beq controls_pan
    cmp a, #$e5
    beq controls_song
    cmp a, #$ed
    beq controls_track
    jmp track_reject
controls_instrument:
    call track_read
    cmp a, #$02
    beq controls_instrument_ok
    cmp a, #$0a
    bne controls_reject
controls_instrument_ok:
    mov $30, a
    ret
controls_song:
    call track_read
    cmp a, #$a0
    beq controls_song_ok
    cmp a, #$50
    bne controls_reject
controls_song_ok:
    mov $31, a
    ret
controls_track:
    call track_read
    cmp a, #$7f
    beq controls_track_ok
    cmp a, #$40
    bne controls_reject
controls_track_ok:
    mov $32, a
    ret
controls_pan:
    call track_read
    cmp a, #$00
    beq controls_pan_ok
    cmp a, #$0a
    beq controls_pan_ok
    cmp a, #$14
    bne controls_reject
controls_pan_ok:
    mov $33, a
    ret
controls_reject:
    jmp track_reject
controls_validate:
    mov a, $23
    cmp a, #$7f
    bne controls_reject
    mov a, $26
    cmp a, #$c9
    beq controls_gate_validate
    mov a, $33
    cmp a, #$0a
    beq controls_pan_valid
    mov a, $30
    cmp a, #$02
    bne controls_reject
    mov a, $31
    cmp a, #$a0
    bne controls_reject
    mov a, $32
    cmp a, #$7f
    bne controls_reject
controls_pan_valid:
    mov a, $22
    cmp a, #$10
    bne controls_reject
    mov a, $31
    cmp a, #$a0
    beq controls_check_track
    mov a, $32
    cmp a, #$7f
    bne controls_reject
    mov a, $30
    cmp a, #$02
    bne controls_reject
    bra controls_gate_validate
controls_check_track:
    mov a, $32
    cmp a, #$7f
    beq controls_gate_validate
    mov a, $30
    cmp a, #$02
    bne controls_reject
controls_gate_validate:
    jmp gate_validate
controls_render_event:
    call render_stop
    mov a, $26
    cmp a, #$c9
    bne controls_render_note
    jmp controls_render_done
controls_render_note:
    mov $2c, #$10
controls_release_wait:
    dec $2c
    mov a, $2c
    bne controls_release_wait
    ; Bounded voice-volume and pan points from sanitized observations.
    mov a, $31
    cmp a, #$a0
    bne controls_low_volume
    mov a, $32
    cmp a, #$7f
    bne controls_low_volume
    mov a, #$07
    bra controls_volume
controls_low_volume:
    mov a, #$01
controls_volume:
    mov $34, a
    mov a, $33
    cmp a, #$0a
    beq controls_center
    cmp a, #$00
    beq controls_right
    mov $f2, #$20
    mov $f3, #$0b
    mov $f2, #$21
    mov $f3, #$00
    bra controls_volume_done
controls_right:
    mov $f2, #$20
    mov $f3, #$00
    mov $f2, #$21
    mov $f3, #$0b
    bra controls_volume_done
controls_center:
    mov a, $34
    mov $f2, #$20
    mov $f3, a
    mov $f2, #$21
    mov $f3, a
controls_volume_done:
    mov $f2, #$24
    mov a, $30
    mov $f3, a
    cmp a, #$02
    bne controls_inst10
    mov $f2, #$25
    mov $f3, #$8f
    mov $f2, #$26
    mov $f3, #$6f
    mov a, $26
    cmp a, #$98
    beq controls_pitch24
    cmp a, #$99
    beq controls_pitch25
    mov $f2, #$22
    mov $f3, #$5c
    mov $f2, #$23
    mov $f3, #$08
    bra controls_keyon
controls_pitch24:
    mov a, #$2c
    bra controls_low_pitch
controls_pitch25:
    mov a, #$6c
controls_low_pitch:
    mov $f2, #$22
    mov $f3, a
    mov $f2, #$23
    mov $f3, #$04
    bra controls_keyon
controls_inst10:
    mov $f2, #$25
    mov $f3, #$8e
    mov $f2, #$26
    mov $f3, #$af
    mov a, $26
    cmp a, #$98
    beq controls_inst10_pitch24
    cmp a, #$99
    beq controls_inst10_pitch25
    mov $f2, #$22
    mov $f3, #$90
    mov $f2, #$23
    mov $f3, #$3e
    bra controls_keyon
controls_inst10_pitch24:
    mov $f2, #$22
    mov $f3, #$39
    mov $f2, #$23
    mov $f3, #$1f
    bra controls_keyon
controls_inst10_pitch25:
    mov $f2, #$22
    mov $f3, #$18
    mov $f2, #$23
    mov $f3, #$21
controls_keyon:
    mov $f2, #$27
    mov $f3, #$b8
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$04
    call gate_start
controls_render_done:
    ret
