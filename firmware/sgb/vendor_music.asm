; SPDX-License-Identifier: GPL-3.0-or-later
; Independent experimental single-channel uploaded music interpreter.
; Owned resident square/triangle; admitted caller-uploaded source 2.
; No vendor program code, resident tables or resident samples.
; DB is a separate diagnostic protocol, never a production advertisement.
.org $0200
vendor_cold:
    mov $d2, #$00
    mov $d4, #$00
    mov $d5, #$00
    mov $d6, #$00
    mov $d7, #$00
    mov $d8, #$00
    mov $d9, #$00
    mov $df, #$7f
    mov $54, #$00
    jmp vendor_publish
vendor_publish:
    call vendor_mute
    .byte $cd, $ef, $bd
    mov $d0, #$00
    mov $d1, #$00
    mov $f6, #$00
    mov $f5, #$db
    mov $f7, #$a5
    mov $f4, #$5a
vendor_arm:
    mov a, $f4
    bne vendor_arm
    mov $f4, #$00
vendor_poll:
    call vendor_service
    bra vendor_poll
vendor_service:
    mov a, $f4
    cmp a, $d0
    beq vendor_service_done
    mov $d0, a
    mov a, $f7
    cmp a, #$01
    beq vendor_loader
    cmp a, #$03
    beq vendor_stop
    cmp a, #$02
    beq vendor_stage
    cmp a, #$00
    bne vendor_bad
    mov a, $f5
    or a, $f6
    bne vendor_bad
    mov a, $d1
    beq vendor_ack
    mov $d3, a
    mov $d1, #$00
    mov a, $d0
    mov $f4, a
    .byte $cd, $ef, $bd
    jmp vendor_prepare
vendor_stage:
    mov a, $f5
    and a, #$c0
    cmp a, #$c0
    beq vendor_bad
    mov $df, #$7f
    mov a, $f5
    and a, #$0c
    cmp a, #$0c
    bne vendor_master_volume
    mov $df, #$00
vendor_master_volume:
    mov $f2, #$0c
    mov a, $df
    mov $f3, a
    mov $f2, #$1c
    mov $f3, a
    call vendor_echo_apply
    mov a, $f6
    beq vendor_ack
    cmp a, #$80
    beq vendor_stop
    cmp a, #$04
    bcs vendor_bad
    mov $d1, a
vendor_ack:
    mov a, $d0
    mov $f4, a
vendor_service_done:
    ret
vendor_stop:
    call vendor_mute
    mov $d1, #$00
    mov $d2, #$01
    mov $f6, #$01
    mov a, $d0
    mov $f4, a
    .byte $cd, $ef, $bd
    jmp vendor_poll
vendor_loader:
    call vendor_mute
    mov a, $d4
    .byte $c5, $02, $05
    mov a, $d5
    .byte $c5, $03, $05
    mov a, $d7
    .byte $c5, $04, $05
    mov a, $d8
    .byte $c5, $05, $05
    mov a, $d9
    .byte $c5, $06, $05
    mov a, $df
    .byte $c5, $08, $05
    mov $d2, #$00
    mov $f5, #$00
    mov $f7, #$00
    .byte $cd, $ef, $bd
    jmp $ffc0
vendor_mute:
    mov $f1, #$80
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f2, #$6c
    mov $f3, #$e0
    ret
vendor_bad:
    call vendor_mute
    mov $d6, #$e2
    mov $d2, #$00
    mov $f5, #$00
    mov $f7, #$00
    mov $f6, #$e2
    mov a, $d0
    mov $f4, a
    .byte $cd, $ef, $bd
vendor_bad_poll:
    bra vendor_bad_poll
.org $0400
vendor_restart:
    mov a, $0502
    mov $d4, a
    mov a, $0503
    mov $d5, a
    mov a, $0504
    mov $d7, a
    mov a, $0505
    mov $d8, a
    mov a, $0506
    mov $d9, a
    mov a, $0508
    mov $df, a
    mov a, $0510
    mov $26, a
    mov a, $0511
    mov $27, a
    mov $d2, #$01
    inc $d9
    call vendor_assets
    jmp vendor_publish
; Host supplies exclusive source end at $0510; no unprovided byte is read.
; Directory/sample locations are separated from score and echo RAM.
.org $0600
.byte $00,$07,$00,$07,$09,$07,$09,$07
.org $0700
.byte $a3,$33,$33,$33,$33,$dd,$dd,$dd,$dd
.byte $a3,$01,$23,$43,$21,$0f,$ed,$cd,$ef
.org $0800
vendor_prepare:
    call vendor_mute
    mov $d2, #$03
    mov $3d, #$00
    call vendor_song
    mov $3d, #$01
    call vendor_setup
    inc $d4
    mov $d2, #$02
    mov $f6, #$03
    call vendor_song
    call vendor_release
    mov $3d, #$00
    inc $d5
    mov $d2, #$01
    mov $f6, #$02
    jmp vendor_poll
vendor_song:
    ; Bounds: 4096 source bytes, 2048 reads/pass, 128 events, eight patterns.
    mov a, $27
    cmp a, #$2b
    bcc vendor_bad
    cmp a, #$3c
    bcs vendor_bad
    cmp a, #$3b
    bne vendor_end_ok
    mov a, $26
    bne vendor_bad
vendor_end_ok:
    mov $28, #$00
    mov $29, #$08
    mov $45, #$00
    mov $46, #$00
    mov $30, #$00
    mov $31, #$7f
    mov $32, #$60
    mov $33, #$0a
    mov $34, #$7f
    mov $35, #$a0
    mov $36, #$02
    mov $37, #$00
    mov $38, #$00
    mov $39, #$00
    mov $3a, #$00
    mov $3b, #$00
    mov $3c, #$00
    mov $41, #$00
    mov $3e, #$00
    mov $4f, #$00
    mov a, $d3
    clrc
    adc a, $d3
    mov $20, a
    dec $20
    dec $20
    mov $21, #$2b
    call vendor_word
    mov a, $43
    mov $20, a
    mov a, $44
    mov $21, a
vendor_phrase:
    call vendor_word
    mov a, $20
    mov $22, a
    mov a, $21
    mov $23, a
    mov a, $43
    or a, $44
    beq vendor_song_done
    inc $45
    mov a, $45
    cmp a, #$09
    bcs vendor_bad
    mov a, $43
    mov $20, a
    mov a, $44
    mov $21, a
    mov $47, #$00
vendor_table:
    call vendor_word
    mov a, $47
    cmp a, #$02
    beq vendor_channel
    mov a, $43
    or a, $44
    bne vendor_bad
    bra vendor_table_next
vendor_channel:
    mov a, $43
    mov $24, a
    mov a, $44
    mov $25, a
vendor_table_next:
    inc $47
    mov a, $47
    cmp a, #$08
    bne vendor_table
    mov a, $24
    or a, $25
    beq vendor_pattern_done
    mov a, $24
    mov $20, a
    mov a, $25
    mov $21, a
    mov $30, #$00
vendor_track:
    call vendor_read
vendor_opcode:
    beq vendor_pattern_done
    cmp a, #$80
    bcs vendor_event
    mov $30, a
    call vendor_read
    cmp a, #$80
    bcs vendor_event
    mov $31, a
    call vendor_read
    cmp a, #$80
    bcc vendor_bad
vendor_event:
    cmp a, #$e0
    bcs vendor_control
    mov $42, a
    cmp a, #$c8
    beq vendor_bad
    cmp a, #$ca
    bcs vendor_bad
    mov a, $30
    beq vendor_bad
    inc $46
    mov a, $46
    cmp a, #$81
    bcs vendor_bad
    mov a, $3d
    beq vendor_track
    call vendor_play
    jmp vendor_track
vendor_pattern_done:
    mov a, $22
    mov $20, a
    mov a, $23
    mov $21, a
    jmp vendor_phrase
vendor_song_done:
    ret
vendor_control:
    cmp a, #$f6
    bne vendor_control_operand
    mov $37, #$00
    call vendor_echo_apply
    jmp vendor_track
vendor_control_operand:
    mov $48, a
    call vendor_read
    mov $49, a
    mov a, $48
    cmp a, #$e0
    beq vendor_instrument
    cmp a, #$e1
    beq vendor_pan
    cmp a, #$e5
    beq vendor_volume
    cmp a, #$e7
    beq vendor_tempo
    cmp a, #$ed
    beq vendor_track_volume
    cmp a, #$f5
    beq vendor_echo_send
    cmp a, #$f7
    beq vendor_echo_setup
    jmp vendor_bad
vendor_instrument:
    mov a, $49
    cmp a, #$02
    beq vendor_instrument_ok
    cmp a, #$0a
    bne vendor_bad
vendor_instrument_ok:
    mov $36, a
    jmp vendor_track
vendor_pan:
    mov a, $49
    cmp a, #$15
    bcs vendor_bad
    mov $33, a
    jmp vendor_track
vendor_volume:
    mov a, $49
    mov $35, a
    jmp vendor_track
vendor_track_volume:
    mov a, $49
    mov $34, a
    jmp vendor_track
vendor_tempo:
    mov a, $49
    beq vendor_bad
    mov $32, a
    jmp vendor_track
vendor_echo_send:
    mov a, $49
    ; Only logical channel 2 exists in this profile. Other send bits cannot
    ; enable an absent voice; retain the channel-2 bit of the score's mask.
    and a, #$04
    mov $37, a
    call vendor_read
    mov $38, a
    call vendor_read
    mov $39, a
    call vendor_echo_apply
    jmp vendor_track
vendor_echo_setup:
    mov a, $49
    cmp a, #$10
    bcs vendor_bad
    mov $3a, a
    call vendor_read
    mov $3b, a
    call vendor_read
    cmp a, #$04
    bcs vendor_bad
    mov $3c, a
    call vendor_echo_apply
    jmp vendor_track
vendor_read:
    call vendor_service
    mov a, $28
    or a, $29
    beq vendor_bad
    mov a, $28
    bne vendor_budget_low
    dec $29
vendor_budget_low:
    dec $28
    mov a, $21
    cmp a, #$2b
    bcc vendor_bad
    cmp a, $27
    bcc vendor_read_ok
    bne vendor_bad
    mov a, $20
    cmp a, $26
    bcs vendor_bad
vendor_read_ok:
    .byte $8d, $00, $f7, $20
    mov $4a, a
    inc $20
    mov a, $20
    bne vendor_read_return
    inc $21
vendor_read_return:
    mov a, $4a
    ret
vendor_word:
    call vendor_read
    mov $43, a
    call vendor_read
    mov $44, a
    ret
vendor_setup:
    mov $f2, #$0c
    mov a, $df
    mov $f3, a
    mov $f2, #$1c
    mov $f3, a
    mov $f2, #$2d
    mov $f3, #$00
    mov $f2, #$3d
    mov $f3, #$00
    mov $f2, #$4d
    mov $f3, #$00
    mov $f2, #$5d
    mov $f3, #$06
    mov $f2, #$6d
    mov $f3, #$80
    mov $f2, #$7d
    mov $f3, #$00
    mov $f2, #$0f
    mov $f3, #$7f
    mov x, #$00
vendor_fir:
    mov a, vendor_fir_addresses+x
    mov $f2, a
    mov $f3, #$00
    .byte $3d
    cmp x, #$07
    bne vendor_fir
    mov $f2, #$6c
    mov $f3, #$00
    mov $fa, #$10
    mov $f1, #$80
    ret
vendor_echo_apply:
    mov a, $3d
    beq vendor_echo_done
    mov $f2, #$4d
    mov a, $37
    mov $f3, a
    mov $f2, #$2c
    mov a, $df
    beq vendor_echo_left_zero
    mov a, $38
vendor_echo_left_zero:
    mov $f3, a
    mov $f2, #$3c
    mov a, $df
    beq vendor_echo_right_zero
    mov a, $39
vendor_echo_right_zero:
    mov $f3, a
    mov $f2, #$7d
    mov a, $3a
    mov $f3, a
    mov $f2, #$0d
    mov a, $3b
    mov $f3, a
    mov a, $3c
    xcn a
    mov $4d, a
    mov $4e, #$00
vendor_echo_fir:
    mov x, $4e
    mov a, vendor_all_fir_addresses+x
    mov $f2, a
    mov a, $4d
    clrc
    adc a, $4e
    mov x, a
    mov a, vendor_owned_filters+x
    mov $f3, a
    inc $4e
    mov a, $4e
    cmp a, #$08
    bne vendor_echo_fir
vendor_echo_done:
    ret
vendor_play:
    mov a, $4f
    bne vendor_timer_started
    mov $f1, #$81
    mov a, $fd
    mov $4f, #$01
vendor_timer_started:
    mov $74, #$00
    mov $75, #$00
    mov $76, #$00
    mov a, $30
    mov $3f, a
    mov $f2, #$5c
    mov $f3, #$04
    mov a, $42
    cmp a, #$c9
    beq vendor_rest
    call vendor_gate_lookup
    mov a, $42
    ; A fresh original pitch scale and velocity/gate curves, not vendor tables.
    and a, #$7f
    mov $4c, a
    clrc
    adc a, $4c
    mov x, a
    mov a, vendor_pitch+x
    mov $60, a
    .byte $3d
    mov a, vendor_pitch+x
    mov $61, a
    mov a, $54
    beq vendor_resident_pitch
    mov a, $36
    cmp a, #$02
    bne vendor_resident_pitch
    mov a, $4c
    cmp a, #$18
    bcc vendor_tuning_ready
    cmp a, #$26
    bcs vendor_tuning_ready
    clrc
    adc a, $4c
    .byte $80, $a8, $30
    mov x, a
    mov a, vendor_measured_pitch+x
    mov $60, a
    .byte $3d
    mov a, vendor_measured_pitch+x
    mov $61, a
vendor_tuning_ready:
    call vendor_tuned_pitch
    mov $f2, #$22
    mov a, $66
    mov $f3, a
    mov $f2, #$23
    mov a, $67
    mov $f3, a
    mov $f2, #$24
    mov $f3, #$02
    mov $f2, #$25
    mov a, $4c3d
    mov $f3, a
    mov $f2, #$26
    mov a, $4c3e
    mov $f3, a
    mov $f2, #$27
    mov a, $4c3f
    mov $f3, a
    jmp vendor_voice_volume
vendor_resident_pitch:
    mov $f2, #$22
    mov a, $60
    mov $f3, a
    mov $f2, #$23
    mov a, $61
    mov $f3, a
    mov $f2, #$24
    mov $f3, #$00
    mov $f2, #$25
    mov $f3, #$8f
    mov $f2, #$26
    mov $f3, #$6f
    mov a, $36
    cmp a, #$02
    beq vendor_voice_volume
    mov $f2, #$24
    mov $f3, #$01
    mov $f2, #$25
    mov $f3, #$8e
    mov $f2, #$26
    mov $f3, #$af
vendor_voice_volume:
    mov a, $35
    .byte $eb, $34, $cf, $dd
    mov $4b, a
    mov a, $31
    and a, #$0f
    mov x, a
    mov a, vendor_velocity+x
    .byte $eb, $4b, $cf, $dd
    mov $4b, a
    mov x, $33
    mov a, vendor_pan_left+x
    .byte $eb, $4b, $cf, $dd
    mov $f2, #$20
    mov $f3, a
    mov a, vendor_pan_right+x
    .byte $eb, $4b, $cf, $dd
    mov $f2, #$21
    mov $f3, a
    mov a, $31
    lsr a
    lsr a
    lsr a
    lsr a
    mov x, a
    mov a, vendor_quantization+x
    .byte $eb, $30, $cf, $dd
    bne vendor_gate_ok
    mov a, #$01
vendor_gate_ok:
    mov $40, a
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$04
    inc $d7
    mov a, $d7
    bne vendor_wait
    inc $d8
    bra vendor_wait
vendor_rest:
    mov $40, #$00
vendor_wait:
    call vendor_service
    mov a, $3e
    bne vendor_pulse
    mov a, $fd
    mov $3e, a
vendor_pulse:
    mov a, $3e
    beq vendor_wait
    dec $3e
    call vendor_gate_pulse
    mov a, $41
    clrc
    adc a, $32
    mov $41, a
    bcc vendor_profile_boundary
    mov a, $74
    bne vendor_duration_tick
    mov a, $40
    beq vendor_gate_off
    dec $40
    mov a, $40
    bne vendor_duration_tick
vendor_gate_off:
    mov $f2, #$5c
    mov $f3, #$04
vendor_duration_tick:
    mov a, $75
    bne vendor_profile_boundary
    dec $3f
    mov a, $3f
    beq vendor_event_done
vendor_profile_boundary:
    mov a, $76
    beq vendor_pulse
    ; This calibrated interval restarts the fractional phase for following
    ; events. Other profiles retain their continuous fractional clock.
    mov $41, #$00
vendor_event_done:
    ret
vendor_fir_addresses:
.byte $1f,$2f,$3f,$4f,$5f,$6f,$7f
vendor_all_fir_addresses:
.byte $0f,$1f,$2f,$3f,$4f,$5f,$6f,$7f
; Original identity and three gentle low-pass responses. Each sums to 127.
; These deliberately do not reproduce the proprietary resident filter bank.
vendor_owned_filters:
.byte $7f,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
.byte $60,$1f,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
.byte $40,$20,$10,$08,$04,$02,$01,$00,$00,$00,$00,$00,$00,$00,$00,$00
.byte $20,$20,$20,$1f,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00,$00
vendor_velocity:
.byte $00,$11,$22,$33,$44,$55,$66,$77,$88,$99,$aa,$bb,$cc,$dd,$ee,$ff
vendor_quantization:
.byte $40,$60,$80,$a0,$c0,$d0,$e0,$f0
vendor_pan_left:
.byte $7f,$79,$72,$6c,$66,$5f,$59,$53,$4c,$46,$40,$39,$33,$2c,$26,$20,$19,$13,$0d,$06,$00
vendor_pan_right:
.byte $00,$06,$0d,$13,$19,$20,$26,$2c,$33,$39,$40,$46,$4c,$53,$59,$5f,$66,$6c,$72,$79,$7f
; Caller asset admission. Trusted host manifest names complete uploaded bytes.
.org $1000
vendor_assets:
    mov $54, #$00
    mov a, $0512
    beq vendor_assets_done
    cmp a, #$01
    bne vendor_bad
    mov a, $4c3c
    cmp a, #$02
    bne vendor_bad
    mov a, $4c40
    mov $57, a
    mov a, $4c41
    mov $56, a
    or a, $57
    beq vendor_bad
    mov a, $4b08
    mov $60, a
    mov a, $4b09
    mov $61, a
    mov a, $4b0a
    mov $62, a
    mov a, $4b0b
    mov $63, a
    mov $64, #$00
vendor_asset_header:
    mov a, $61
    cmp a, #$3b
    bcc vendor_bad
    cmp a, #$4b
    bcs vendor_bad
    mov a, $60
    clrc
    adc a, #$09
    mov $65, a
    mov a, $61
    adc a, #$00
    mov $66, a
    mov a, $0514
    mov $67, a
    mov a, $66
    cmp a, $67
    bcc vendor_asset_fits
    bne vendor_bad
    mov a, $0513
    mov $67, a
    mov a, $65
    cmp a, $67
    bcc vendor_asset_fits
    beq vendor_asset_fits
    jmp vendor_bad
vendor_asset_fits:
    mov a, $60
    cmp a, $62
    bne vendor_asset_read
    mov a, $61
    cmp a, $63
    bne vendor_asset_read
    mov $64, #$01
vendor_asset_read:
    .byte $8d, $00, $f7, $60
    mov $67, a
    and a, #$01
    bne vendor_asset_end
    mov a, $67
    and a, #$02
    bne vendor_bad
    mov a, $65
    mov $60, a
    mov a, $66
    mov $61, a
    jmp vendor_asset_header
vendor_asset_end:
    mov a, $67
    and a, #$02
    beq vendor_asset_copy
    mov a, $64
    beq vendor_bad
vendor_asset_copy:
    ; Every decoded header fits the admitted extent, and the loop names a
    ; visited header for a looping END. A one-shot loop word is unused.
    ; Prefix/trailing bytes are permitted but never decoded.
    .byte $cd, $00
vendor_asset_directory:
    mov a, $4b08+x
    .byte $d5, $08, $06, $3d
    cmp x, #$04
    bne vendor_asset_directory
    mov $54, #$01
vendor_assets_done:
    ret
; Authored pitch curve scaled by the uploaded big-endian tuning word / $0400.
; Full unsigned product, floor division and saturation; no wrap to low pitches.
vendor_tuned_pitch:
    mov $62, #$00
    mov $63, #$00
    mov a, $56
    mov $64, a
    mov a, $57
    mov $65, a
    mov $66, #$00
    mov $67, #$00
    mov $68, #$00
    mov $69, #$00
    mov $6a, #$10
vendor_multiply:
    .byte $4b, $65, $6b, $64
    bcc vendor_product_shift
    mov a, $66
    clrc
    adc a, $60
    mov $66, a
    mov a, $67
    adc a, $61
    mov $67, a
    mov a, $68
    adc a, $62
    mov $68, a
    mov a, $69
    adc a, $63
    mov $69, a
vendor_product_shift:
    .byte $0b, $60, $2b, $61, $2b, $62, $2b, $63
    dec $6a
    mov a, $6a
    bne vendor_multiply
    mov $6a, #$0a
vendor_pitch_divide:
    .byte $4b, $69, $6b, $68, $6b, $67, $6b, $66
    dec $6a
    mov a, $6a
    bne vendor_pitch_divide
    mov a, $68
    or a, $69
    bne vendor_pitch_clamp
    mov a, $67
    cmp a, #$40
    bcc vendor_pitch_ready
vendor_pitch_clamp:
    mov $66, #$ff
    mov $67, #$3f
vendor_pitch_ready:
    ret
; Calibrated owned-fixture profiles, not a general original timing law.
; 70/71 gate pulses; 72/73 event pulses; 74 gate flag; 75 end flag; 76 due.
.org $1200
vendor_gate_lookup:
    mov $74, #$00
    mov $75, #$00
    mov $76, #$00
    mov $78, #$00
vendor_gate_record:
    mov x, $78
    mov a, vendor_gate_profiles+x
    cmp a, $32
    bne vendor_gate_next
    .byte $3d
    mov a, vendor_gate_profiles+x
    cmp a, $31
    bne vendor_gate_next
    .byte $3d
    mov a, vendor_gate_profiles+x
    cmp a, $30
    bne vendor_gate_next
    .byte $3d
    mov a, vendor_gate_profiles+x
    mov $70, a
    .byte $3d
    mov a, vendor_gate_profiles+x
    mov $71, a
    .byte $3d
    mov a, vendor_gate_profiles+x
    mov $72, a
    .byte $3d
    mov a, vendor_gate_profiles+x
    mov $73, a
    or a, $72
    beq vendor_gate_loaded
    mov $75, #$01
vendor_gate_loaded:
    mov $74, #$01
    ret
vendor_gate_next:
    mov a, $78
    clrc
    adc a, #$07
    mov $78, a
    cmp a, #$0e
    bcc vendor_gate_record
    ret
vendor_gate_pulse:
    mov a, $74
    beq vendor_calibrated_done
    mov a, $70
    or a, $71
    beq vendor_calibrated_end
    mov a, $70
    bne vendor_gate_decrement
    dec $71
vendor_gate_decrement:
    dec $70
    mov a, $70
    or a, $71
    bne vendor_calibrated_end
    mov $f2, #$5c
    mov $f3, #$04
    call vendor_gate_clear
vendor_calibrated_end:
    mov a, $75
    beq vendor_calibrated_done
    mov a, $72
    bne vendor_end_decrement
    dec $73
vendor_end_decrement:
    dec $72
    mov a, $72
    or a, $73
    bne vendor_calibrated_done
    mov $76, #$01
vendor_calibrated_done:
    ret
vendor_release:
    mov $f1, #$80
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    ; Hold across every DSP key-off polling phase, then clear the latch.
    .byte $cd, $0c
vendor_release_hold:
    .byte $1d
    cmp x, #$00
    bne vendor_release_hold
    mov $f3, #$00
    ; Original completion observations silence echo returns without resetting
    ; the DSP. Keep the echo RAM and decoder/envelope release running.
    mov $f2, #$2c
    mov $f3, #$00
    mov $f2, #$3c
    mov $f3, #$00
    ret
vendor_gate_clear:
    .byte $cd, $44
vendor_gate_hold:
    .byte $1d
    cmp x, #$00
    bne vendor_gate_hold
    mov $f3, #$00
    ret
vendor_gate_profiles:
; tempo, articulation, duration, gate lo/hi, duration lo/hi (zero = fractional).
.byte $2d,$7d,$60,$16,$02,$22,$02
.byte $60,$7f,$10,$25,$00,$00,$00
vendor_measured_pitch:
; Filled from bounded owned-fixture DSP measurements; no private table bytes.
vendor_pitch:
; Filled by the source builder from an explicitly authored equal-temperament scale.
