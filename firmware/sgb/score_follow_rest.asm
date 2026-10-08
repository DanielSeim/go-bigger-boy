; SPDX-License-Identifier: GPL-3.0-or-later
; $B8 immediate following-rest mode; $B9 measured rest pulse profile.
follow_rest_guard:
    mov a, $12
    cmp a, #$60
    bne follow_rest_bad
    mov a, $9c
    clrc
    adc a, #$01
    mov x, a
    mov a, $4b00+x
    cmp a, #$0c
    bne follow_rest_bad
    mov a, $30
    clrc
    adc a, #$40
    mov x, a
    mov a, $4200+x
    cmp a, #$08
    beq follow_rest_duration
    cmp a, #$10
    bne follow_rest_bad
follow_rest_duration:
    mov a, $4a00+x
    bne follow_rest_bad
    mov $b8, #$01
    mov $b5, #$01
    ret
follow_rest_bad:
    jmp pair_reject
follow_rest_gate:
    call gates_event
    mov a, $b8
    beq follow_rest_return
    mov a, $23
    cmp a, #$02
    bne follow_rest_return
    mov a, $b6
    beq follow_rest_return
    ; The first rest emits no pitch or KON bit. Its measured profile must
    ; nevertheless arm the boundary note that survives into this KON group.
    mov a, $b9
    mov $78, a
follow_rest_return:
    ret
