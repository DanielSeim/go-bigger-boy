; SPDX-License-Identifier: GPL-3.0-or-later
; $C3 marks only the measured held-127 direct return before final readiness.
direct_return_shape:
    mov a, $4281
    cmp a, #$c9
    beq direct_return_rest_shape
    cmp a, #$a0
    bne direct_return_shape_bad
    mov a, $4400
    cmp a, #$7f
    bne direct_return_shape_bad
    mov a, $4280
    cmp a, #$08
    beq direct_return_duration
    cmp a, #$10
    bne direct_return_shape_bad
direct_return_duration:
    mov $bf, a
    mov a, $4282
    cmp a, $bf
    bne direct_return_shape_bad
    mov $c3, #$01
    bra direct_return_shape_ok
direct_return_rest_shape:
    mov a, $4280
    cmp a, #$04
    beq direct_return_shape_ok
    cmp a, #$08
    bne direct_return_shape_bad
direct_return_shape_ok:
    mov a, #$01
    ret
direct_return_shape_bad:
    mov a, #$00
    ret
