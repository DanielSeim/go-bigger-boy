; SPDX-License-Identifier: GPL-3.0-or-later
; Only the qualified returning A0 writes pitch before actual volume.
direct_return_voice:
    mov a, $c3
    beq direct_return_voice_old
    mov a, $9c
    cmp a, #$02
    bne direct_return_voice_old
    mov a, $23
    cmp a, #$02
    bne direct_return_voice_old
    mov a, $26
    cmp a, #$a0
    bne direct_return_voice_old
    call duet_voice
    jmp pending_mix
direct_return_voice_old:
    call pending_mix
    jmp duet_voice
