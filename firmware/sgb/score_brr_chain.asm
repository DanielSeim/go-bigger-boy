; SPDX-License-Identifier: GPL-3.0-or-later
; CF-only uploaded 192-byte object $5000..50BF. Sources 2/3 start at
; $5040/$5080. Counts at $5010/$5011 admit 1..4 nine-byte blocks each.
; Intermediate headers B0; final header B3. Filter/range remain owned/fixed.
; Loops must match a visited header in their own sample. All padding is zero.
; Validation scratch E0=base, E1=remaining, E2=loop low, E7=header cursor,
; E8=window end, E9=loop-found; disjoint from runtime E3..E6.
.org $1800
uploaded_timing_init:
    .byte $cd, $00 ; MOV X,#0
brr_descriptors_check:
    mov a, $5000+x
    mov $df, a
    mov a, brr_descriptors+x
    cmp a, $df
    bne brr_bad
    .byte $3d ; INC X
    cmp x, #$08
    bne brr_descriptors_check
    mov a, $5008
    cmp a, #$40
    bne brr_bad
    mov a, $5009
    cmp a, #$50
    bne brr_bad
    mov a, $500b
    cmp a, #$50
    bne brr_bad
    mov a, $500c
    cmp a, #$80
    bne brr_bad
    mov a, $500d
    cmp a, #$50
    bne brr_bad
    mov a, $500f
    cmp a, #$50
    bne brr_bad
    mov a, $5010
    call brr_count
    mov a, $5011
    call brr_count
    .byte $cd, $12
brr_reserved:
    mov a, $5000+x
    bne brr_bad
    .byte $3d
    cmp x, #$40
    bne brr_reserved
    mov $e0, #$40
    mov a, $5010
    mov $e1, a
    mov a, $500a
    mov $e2, a
    call brr_sample
    mov $e0, #$80
    mov a, $5011
    mov $e1, a
    mov a, $500e
    mov $e2, a
    call brr_sample
    call timing_init
    ret
brr_bad:
    jmp bridge_bad
brr_count:
    cmp a, #$01
    bcc brr_bad
    cmp a, #$05
    bcs brr_bad
    ret
brr_sample:
    mov a, $e0
    mov $e7, a
    clrc
    adc a, #$40
    mov $e8, a
    mov $e9, #$00
brr_block:
    mov a, $e7
    cmp a, $e2
    bne brr_header
    mov $e9, #$01
brr_header:
    mov x, $e7
    mov a, $5000+x
    dec $e1
    mov $df, a
    mov a, $e1
    beq brr_last
    mov a, $df
    cmp a, #$b0
    bne brr_bad
    bra brr_next
brr_last:
    mov a, $df
    cmp a, #$b3
    bne brr_bad
brr_next:
    mov a, $e7
    clrc
    adc a, #$09
    mov $e7, a
    mov a, $e1
    bne brr_block
    mov a, $e9
    beq brr_bad
    mov x, $e7
brr_padding:
    mov a, $5000+x
    bne brr_bad
    .byte $3d
    mov a, x
    cmp a, $e8
    bne brr_padding
    ret
brr_descriptors:
    .byte $02, $8f, $6f, $b8, $03, $8f, $6f, $b8
