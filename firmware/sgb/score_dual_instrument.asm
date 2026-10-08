; SPDX-License-Identifier: GPL-3.0-or-later
; CE-only two descriptors and two owned single-block samples at $5000..503F.
; Ordered E0 IDs use $6000..7FFF: one page per prefix ordinal (max 32),
; indexed by the native expanded event slot. Retain $4A00 prefix counts.
; E3 = descriptor offset; E4/E5 = cache pointer; E6 = render prefix ordinal.
; No E0 means no descriptor writes: DSP state inherits independently per voice.
.org $1800
uploaded_timing_init:
    .byte $cd, $00 ; MOV X,#0
dual_validate:
    mov a, $5000+x
    mov $df, a
    mov a, dual_structure+x
    cmp a, $df
    bne dual_bad
    .byte $3d ; INC X
    cmp x, #$20
    bne dual_validate
    mov a, $5020
    cmp a, #$b3
    bne dual_bad
    mov a, $5030
    cmp a, #$b3
    bne dual_bad
    .byte $cd, $29
dual_padding2:
    mov a, $5000+x
    bne dual_bad
    .byte $3d
    cmp x, #$30
    bne dual_padding2
    .byte $cd, $39
dual_padding3:
    mov a, $5000+x
    bne dual_bad
    .byte $3d
    cmp x, #$40
    bne dual_padding3
    call timing_init
    ret
dual_bad:
    jmp bridge_bad
uploaded_setup:
    call poly_setup
    mov $f2, #$5d
    mov $f3, #$50
    ret

dual_end_cache:
    call reselect_cache
    call mix_cache
    ret

dual_instrument:
    mov a, $4d
    cmp a, #$02
    beq dual_id_ok
    cmp a, #$03
    beq dual_id_ok
    jmp pair_reject
dual_id_ok:
    mov a, $a4
    cmp a, #$20
    bcc dual_cache_id
    jmp pair_reject
dual_cache_id:
    clrc
    adc a, #$60
    mov $e5, a
    mov a, $47
    mov $e4, a
    .byte $cd, $00
    mov a, $4d
    .byte $c7, $e4 ; MOV [$E4+X],A
    inc $a4
    ret

dual_descriptor:
    mov a, $e6
    clrc
    adc a, #$60
    mov $e5, a
    mov a, $63
    mov $e4, a
    .byte $cd, $00
    .byte $e7, $e4 ; MOV A,[$E4+X]
    cmp a, #$02
    beq dual_descriptor2
    cmp a, #$03
    beq dual_descriptor3
    jmp pair_reject
dual_descriptor2:
    mov $e3, #$00
    bra dual_descriptor_ready
dual_descriptor3:
    mov $e3, #$04
dual_descriptor_ready:
    inc $e6
    ret

dual_field:
    mov a, $a7
    clrc
    adc a, $ab
    mov $f2, a
    mov a, $ab
    clrc
    adc a, $e3
    mov x, a
    mov a, $5000+x
    mov $f3, a
    ret

dual_structure:
    .byte $02, $8f, $6f, $b8, $03, $8f, $6f, $b8
    .byte $20, $50, $20, $50, $30, $50, $30, $50
    .byte $00, $00, $00, $00, $00, $00, $00, $00
    .byte $00, $00, $00, $00, $00, $00, $00, $00
