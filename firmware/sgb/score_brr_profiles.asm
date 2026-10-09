; SPDX-License-Identifier: GPL-3.0-or-later
; D2-only uploaded 192-byte object $5000..50BF. Sources 2/3 start at
; $5040/$5080. Counts at $5010/$5011 admit 1..4 nine-byte blocks each.
; Each header admits range 8/11 and filter 0..3. Flags: intermediate 0,
; terminal 3 looping or 1 one-shot; checked independently per block.
; Loops must match a visited header in their own sample. All padding is zero.
; Modes $5013/$5017: 0 looping, 1 one-shot; one-shot loop pointer = start.
; Tuning $5012/$5016: 0 normal, 1 half pitch; EC/ED inherit per voice.
; Validation scratch E0=base, E1=remaining, E2=loop low, E7=header cursor,
; E8=window end, E9=loop-found, EA=mode; disjoint from runtime E3..E6.
.org $1800
uploaded_timing_init:
    mov a, $5000
    cmp a, #$02
    bne brr_bad
    mov a, $5004
    cmp a, #$03
    bne brr_bad
    .byte $cd, $00
    call profile_validate
    .byte $cd, $04
    call profile_validate
    mov a, $5012
    cmp a, #$02
    bcs brr_bad
    mov a, $5016
    cmp a, #$02
    bcs brr_bad
    mov a, $5013
    cmp a, #$02
    bcs brr_bad
    mov a, $5017
    cmp a, #$02
    bcs brr_bad
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
    cmp x, #$12
    beq profile_reserved_next
    cmp x, #$13
    beq profile_reserved_next
    cmp x, #$16
    beq profile_reserved_next
    cmp x, #$17
    beq profile_reserved_next
    mov a, $5000+x
    bne brr_bad
profile_reserved_next:
    .byte $3d
    cmp x, #$40
    bne brr_reserved
    mov a, $5013
    mov $ea, a
    mov $e0, #$40
    mov a, $5010
    mov $e1, a
    mov a, $500a
    mov $e2, a
    call brr_sample
    mov a, $5017
    mov $ea, a
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
    mov a, $ea
    beq brr_sample_ready
    mov a, $e2
    cmp a, $e0
    bne brr_bad
brr_sample_ready:
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
    mov $df, a
    and a, #$f0
    cmp a, #$80
    beq brr_range_ready
    cmp a, #$b0
    bne brr_bad
brr_range_ready:
    mov a, $df
    and a, #$03
    mov $df, a
    dec $e1
    mov a, $e1
    beq brr_last
    mov a, $df
    cmp a, #$00
    bne brr_bad
    bra brr_next
brr_last:
    mov a, $ea
    beq brr_last_loop
    mov a, $df
    cmp a, #$01
    bne brr_bad
    bra brr_next
brr_last_loop:
    mov a, $df
    cmp a, #$03
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

uploaded_setup:
    call poly_setup
    mov $f2, #$5d
    mov $f3, #$50
    mov a, $5012
    mov $ec, a
    mov $ed, a
    .byte $cd, $00
profile_setup_fields:
    mov a, x
    clrc
    adc a, #$24
    mov $f2, a
    mov a, $5000+x
    mov $f3, a
    mov $df, a
    mov a, x
    clrc
    adc a, #$34
    mov $f2, a
    mov a, $df
    mov $f3, a
    .byte $3d
    cmp x, #$04
    bne profile_setup_fields
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
    ; Descriptor offsets 0/4 select tuning bytes $5012/$5016.
    mov a, $e3
    mov x, a
    mov a, $5012+x
    mov $df, a
    mov x, $23
    mov a, $df
    .byte $d5, $ea, $00 ; MOV $00EA+X,A: voice-local EC/ED
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


; Bounded ADSR fields with fixed B8 GAIN, or direct GAIN 40/60 with ADSR off.
profile_validate:
    mov a, $5001+x
    beq profile_direct_gain
    cmp a, #$8f
    beq profile_decay
    cmp a, #$8a
    beq profile_decay
    cmp a, #$ff
    bne brr_bad
profile_decay:
    mov a, $5002+x
    cmp a, #$6f
    beq profile_gain
    cmp a, #$4c
    bne brr_bad
profile_gain:
    mov a, $5003+x
    cmp a, #$b8
    bne brr_bad
    ret
profile_direct_gain:
    mov a, $5002+x
    bne brr_bad
    mov a, $5003+x
    cmp a, #$40
    beq profile_direct_valid
    cmp a, #$60
    bne brr_bad
profile_direct_valid:
    ret
; Base pitch is in X:$50. Halve the entire word, preserving normal tuning.
profile_pitch:
    mov a, x
    mov $eb, a
    mov x, $23
    mov a, $00ea+x
    beq profile_pitch_done
    mov a, $eb
    lsr a
    mov $eb, a
    .byte $6b, $50 ; ROR $50 with carry from high byte
profile_pitch_done:
    mov x, $eb
    ret
