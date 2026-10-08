; SPDX-License-Identifier: GPL-3.0-or-later
short_return_shape:
    mov a, $4282
    cmp a, #$08
    beq short_return_shape_ok
    cmp a, #$10
    bne short_return_shape_bad
short_return_shape_ok:
    mov $c3, #$01
    mov a, #$01
    ret
short_return_shape_bad:
    jmp direct_return_shape_bad
