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
    mov a, $f4
    cmp a, $d0
    beq bridge_poll
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
    mov $d1, #$00
    mov $f6, #$03
    mov a, $d0
    mov $f4, a
    mov $d2, #$02
    mov $12, #$60
    mov $20, #$00
    mov $8e, #$08
    jmp $0800
bridge_stage:
    mov a, $d2
    beq bridge_bad
    mov a, $f5
    bne bridge_bad
    mov a, $f6
    cmp a, #$01
    bne bridge_bad
    mov $d1, #$01
bridge_ack:
    mov a, $d0
    mov $f4, a
    jmp bridge_poll
bridge_stop:
    mov $d1, #$00
    jmp bridge_ack
bridge_loader:
    mov $f5, #$00
    mov $f7, #$00
    mov $f2, #$6c
    mov $f3, #$e0
    mov $f1, #$80
    jmp $ffc0
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
    mov $f1, #$80
    mov $14, #$02
    mov $f6, #$02
; Discard playback call frames; subsequent selection re-enters the engine.
    .byte $cd, $ef
    .byte $bd
    jmp bridge_poll
.org $0400
bridge_restart:
    mov $f5, #$00
    mov $f7, #$00
    mov $d2, #$00
    mov $12, #$60
    mov $20, #$00
    mov $8e, #$08
    jmp $0800
