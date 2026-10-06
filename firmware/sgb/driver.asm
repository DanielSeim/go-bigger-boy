; SPDX-License-Identifier: GPL-3.0-or-later
; Original diagnostic SPC700 driver, entry $0200. Never reads reference assets.
; Ports: host token, effect A, effect B; SPC echoes token only after DSP writes.
; Host validates codes: 00 keep voice; 01 trigger our tone; 80 stop it.
; This is an original test instrument, not the proprietary SGB sound bank.
    mov $f2, #$6c
    mov $f3, #$20
    mov $f2, #$0c
    mov $f3, #$60
    mov $f2, #$1c
    mov $f3, #$60
    mov $f2, #$5d
    mov $f3, #$05
; Voice 6: original square BRR at directory entry 0.
    mov $f2, #$60
    mov $f3, #$40
    mov $f2, #$61
    mov $f3, #$40
    mov $f2, #$62
    mov $f3, #$00
    mov $f2, #$63
    mov $f3, #$10
    mov $f2, #$64
    mov $f3, #$00
    mov $f2, #$65
    mov $f3, #$00
    mov $f2, #$67
    mov $f3, #$60
; Voice 5: original stepped triangle BRR at directory entry 1.
    mov $f2, #$50
    mov $f3, #$40
    mov $f2, #$51
    mov $f3, #$40
    mov $f2, #$52
    mov $f3, #$00
    mov $f2, #$53
    mov $f3, #$08
    mov $f2, #$54
    mov $f3, #$01
    mov $f2, #$55
    mov $f3, #$00
    mov $f2, #$57
    mov $f3, #$60
    mov a, $f4
    mov $10, a
    mov $f7, #$a5
    mov $f4, #$5a
poll:
    mov a, $f4
    cmp a, $10
    beq poll
    mov $10, a
    mov $11, #$00
    mov $12, #$00
    mov a, $f5
    beq effect_b
    bmi stop_a
    mov a, $11
    or a, #$40
    mov $11, a
    bra effect_b
stop_a:
    mov a, $12
    or a, #$40
    mov $12, a
effect_b:
    mov a, $f6
    beq apply
    bmi stop_b
    mov a, $11
    or a, #$20
    mov $11, a
    bra apply
stop_b:
    mov a, $12
    or a, #$20
    mov $12, a
apply:
    mov $f2, #$5c
    mov a, $12
    mov $f3, a
    mov $f2, #$4c
    mov a, $11
    mov $f3, a
    mov a, $10
    mov $f4, a
    bra poll
