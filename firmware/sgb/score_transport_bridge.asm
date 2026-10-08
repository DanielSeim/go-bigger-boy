; SPDX-License-Identifier: GPL-3.0-or-later
; Opt-in CB mailbox: score 1 only, no effects or attributes. Engine at $0800.
; D0 = last token, D1 = staged score, D2 = cold/validated/rendering (0/1/2).
; These direct-page bytes are reserved outside the engine's scratch region.
.org $0200
bridge_cold:
    mov $f1, #$80
    mov $f2, #$6c
    mov $f3, #$e0
    mov $d2, #$00
    mov $d3, #$00
    mov $d4, #$00
    mov $d5, #$00
    mov $d6, #$00
    jmp bridge_publish
bridge_publish:
; Discard validation call frames before waiting for fresh ownership.
    .byte $cd, $ef
    .byte $bd
; MOV X,#EF; MOV SP,X (raw encodings outside the assembler's MOV subset).
    mov $d0, #$00
    mov $d1, #$00
    mov $f6, #$00
    mov a, $d2
    beq bridge_signature
    mov $f6, #$01
bridge_signature:
    mov $f5, #$cb
    mov $f7, #$a5
    mov $f4, #$5a
bridge_arm:
    mov a, $f4
    bne bridge_arm
    mov $f4, #$00
bridge_poll:
    call bridge_service
    bra bridge_poll
bridge_service:
    mov a, $f4
    cmp a, $d0
    beq bridge_no_command
    mov $d0, a
    mov a, $f7
    cmp a, #$01
    beq bridge_loader
    cmp a, #$02
    beq bridge_stage
    cmp a, #$03
    beq bridge_stop
    cmp a, #$00
    bne bridge_bad
    mov a, $f5
    or a, $f6
    bne bridge_bad
    mov a, $d1
    beq bridge_ack
    mov a, #$02
    call bridge_observe
    call bridge_mute
    mov $d1, #$00
    mov $f6, #$03
    mov a, $d0
    mov $f4, a
    mov $d2, #$02
    mov $12, #$60
    mov $20, #$00
    mov $8e, #$08
    .byte $cd, $ef
    .byte $bd
    jmp $0800
bridge_stage:
    mov a, $d2
    beq bridge_bad
    mov a, $f5
    bne bridge_bad
    mov a, $f6
    cmp a, #$80
    beq bridge_stop
    cmp a, #$01
    bne bridge_bad
    mov $d1, #$01
bridge_ack:
    mov a, $d0
    mov $f4, a
bridge_no_command:
    mov a, $d2
    cmp a, #$02
    beq bridge_timer
    mov a, #$00
    ret
bridge_timer:
    mov a, $fd
    ret
bridge_stop:
    mov a, #$01
    call bridge_observe
    call bridge_mute
    mov $d1, #$00
    mov $d2, #$01
    mov $14, #$02
    mov $f6, #$01
    jmp bridge_ack
bridge_loader:
    mov a, #$04
    call bridge_observe
    call bridge_save_observations
    mov $f5, #$00
    mov $f7, #$00
    call bridge_mute
    .byte $cd, $ef
    .byte $bd
    jmp $ffc0
; Diagnostic observations: D3 active-interruption mask, D4 positive ENVX mask,
; D5 last interrupted score tick, D6 count, D7 scratch action bit.
bridge_observe:
    mov $d7, a
    mov a, $d2
    cmp a, #$02
    bne bridge_observe_return
    mov a, $d3
    or a, $d7
    mov $d3, a
    mov a, $10
    mov $d5, a
    inc $d6
    mov $f2, #$28
    mov a, $f3
    beq bridge_observe_return
    mov a, $d4
    or a, $d7
    mov $d4, a
bridge_observe_return:
    ret
; The IPL clears direct page. Preserve diagnostic counters in unused RAM.
bridge_save_observations:
    mov a, $d3
    .byte $c5, $00, $05
    mov a, $d4
    .byte $c5, $01, $05
    mov a, $d5
    .byte $c5, $02, $05
    mov a, $d6
    .byte $c5, $03, $05
    ret
bridge_mute:
    mov $f1, #$80
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f2, #$6c
    mov $f3, #$e0
    ret
bridge_bad:
    mov $f5, #$00
    mov $f7, #$00
    mov $f6, #$e2
    mov $f2, #$6c
    mov $f3, #$e0
bridge_bad_halt:
    bra bridge_bad_halt
bridge_start:
    mov a, $d2
    cmp a, #$02
    beq bridge_render
    mov $d2, #$01
    jmp bridge_publish
bridge_render:
    call poly_setup
    ret
bridge_complete:
    mov $d2, #$01
    mov $f1, #$80
    mov $14, #$02
    mov $f6, #$02
; Discard playback call frames; subsequent selection re-enters the engine.
    .byte $cd, $ef
    .byte $bd
    jmp bridge_poll
.org $0400
bridge_restart:
    mov a, $0500
    mov $d3, a
    mov a, $0501
    mov $d4, a
    mov a, $0502
    mov $d5, a
    mov a, $0503
    mov $d6, a
    mov $f5, #$00
    mov $f7, #$00
    mov $d2, #$00
    mov $12, #$60
    mov $20, #$00
    mov $8e, #$08
    jmp $0800
