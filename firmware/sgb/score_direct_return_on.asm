; SPDX-License-Identifier: GPL-3.0-or-later
; The measured peer has already released and settled before direct return.
direct_return_on:
    mov a, $c3
    beq direct_return_on_old
    mov a, $9c
    cmp a, #$02
    bne direct_return_on_old
    mov a, $51
    cmp a, #$04
    bne direct_return_on_old
    mov $71, #$00
    mov $f2, #$5c
    mov $f3, #$00
direct_return_on_old:
    jmp poly_on
