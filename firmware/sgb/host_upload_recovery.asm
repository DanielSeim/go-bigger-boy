; SPDX-License-Identifier: GPL-3.0-or-later
; D5-only. $56 playback blocked, $57 rejections, $59 suppressed SOUND,
; The polling loop has native stack $01FF; rejection discards preflight frames.
recovery_poll:
    lda $24
    beq recovery_poll_done
    lda $2141
    cmp #$d5
    bne recovery_poll_done
    lda $2142
    cmp #$e2
    bne recovery_poll_done
    lda $2143
    cmp #$a5
    bne recovery_poll_done
    ; IPL's final jump echo is the SPC's consumed token, not our loader request.
    ; Base the next command on it so a small final chunk cannot alias a retry.
    lda $2140
    sta $23
    inc $57
    lda #$01
    sta $56
    stz $24
    lda #$02
    sta $27
    lda #$09
    sta $20
recovery_poll_done:
    rts
recovery_sound_check:
    lda $56
    ora $24
    beq recovery_sound_allowed
    inc $59
    .byte $38 ; SEC: caller drops the packet without touching SPC input ports.
    rts
recovery_sound_allowed:
    clc
    rts
recovery_reject:
    ; Discard a possible atomic_block/complete call frame before returning to poll.
    ldx #$01ff
    .byte $9a ; TXS: discard nested preflight calls at the polling boundary.
    sep #$20
    lda #$07
    sta $20
    stz $2141
    stz $2142
    lda #$04
    sta $2143
    inc $23
    lda $23
    sta $2140
    jsr wait_echo
    inc $57
    lda #$01
    sta $56
    stz $24
    lda #$09
    sta $20
    jmp poll
