; SPDX-License-Identifier: GPL-3.0-or-later
; Original position-independent SPC driver. Legacy v4; validated scores v5/v6/v7/v8/v9/v10.
; Resident code is assembled at $1000; $0200 is its legacy entry trampoline.
; Control port 3: 0 effects, 1 return to IPL, 2 stage attributes/score from ports 1/2, 3 silence all.
; Effects 00 retrigger remembered instrument, 01..05 select original preset, 80 stop and forget.
    mov $f2, #$6c
    mov $f3, #$20
    mov $f2, #$0c
    mov $f3, #$60
    mov $f2, #$1c
    mov $f3, #$60
    mov $f2, #$5d
    mov $f3, #$05
    mov $f2, #$60
    mov $f3, #$40
    mov $f2, #$61
    mov $f3, #$40
    mov $f2, #$62
    mov $f3, #$00
    mov $f2, #$63
    mov $f3, #$08
    mov $f2, #$64
    mov $f3, #$00
    mov $f2, #$65
    mov $f3, #$00
    mov $f2, #$67
    mov $f3, #$00
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
; Fixed direct-page tables make code relocatable without absolute references.
    mov $d0, #$08
    mov $d1, #$0c
    mov $d2, #$10
    mov $d3, #$18
    mov $d4, #$40
    mov $d5, #$28
    mov $d6, #$10
    mov $d8, #$00
    mov $d9, #$02
    mov $da, #$03
    mov $db, #$03
    mov $dc, #$01
    mov $e1, #$01
    mov $e2, #$02
    mov $e3, #$03
    mov $e4, #$01
    mov $e5, #$00
    mov $1b, #$00
    mov $1e, #$00
    mov $1f, #$00
    mov $43, #$40
    mov $44, #$10
    mov $45, #$ef
    mov $48, #$00
    mov $4a, #$20
; Music uses voice 4 (plus voice 3 for GBS3/GBS4/GBS5/GBS6), separate from effect voices 6/5.
    mov $f2, #$40
    mov $f3, #$30
    mov $f2, #$41
    mov $f3, #$30
    mov $f2, #$42
    mov $f3, #$00
    mov $f2, #$44
    mov $f3, #$01
    mov $f2, #$45
    mov $f3, #$00
    mov $f2, #$47
    mov $f3, #$00
    mov $f2, #$37
    mov $f3, #$00
    mov $13, #$00
    mov $14, #$00
    mov $15, #$00
    mov $16, #$60
    mov $17, #$60
    mov $18, #$00
    mov $1a, #$40
; Timer 0: 128 SPC clocks prescale, target 128 -> 16 ms at 1.024 MHz.
    mov $fa, #$80
    mov $f1, #$81
    mov $f5, #$c4
    mov a, $30
    beq publish_ready
    mov $f5, #$c5
    cmp a, #$01
    beq publish_ready
    mov $f5, #$c6
    cmp a, #$02
    beq publish_ready
    mov $f5, #$c7
    cmp a, #$03
    beq publish_ready
    mov $f5, #$c8
    cmp a, #$04
    beq publish_ready
    mov $f5, #$c9
    cmp a, #$05
    beq publish_ready
    mov $f5, #$ca
publish_ready:
    mov $f7, #$a5
    mov $f4, #$5a
await_arm:
    mov a, $f4
    cmp a, #$00
    bne await_arm
    mov $10, #$00
    mov $f4, #$00
poll:
    mov a, $fd
    beq token
    mov $19, a
tick:
    call $0640
    call $0700
; Instrument A decays by four direct-gain units per physical timer tick.
    mov a, $14
    beq fade
    clrc
    adc a, #$fc
    mov $14, a
    mov $f2, #$67
    mov $f3, a
fade:
    mov a, $16
    cmp a, $17
    beq tick_done
    bmi fade_up
    clrc
    adc a, #$f8
    bra fade_write
fade_up:
    clrc
    adc a, #$08
fade_write:
    mov $16, a
    mov $f2, #$0c
    mov $f3, a
    mov $f2, #$1c
    mov $f3, a
tick_done:
    dec $19
    bne tick
token:
    mov a, $f4
    cmp a, $10
    bne command
    bra poll
command:
    mov $10, a
    mov a, $f7
    cmp a, #$01
    bne check_stage
; Loader return stops voices and timer, clears all readiness fields.
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f4, #$00
    mov $f5, #$00
    mov $f7, #$00
    mov $f1, #$80
    mov $30, #$00
    jmp $ffc0
return_top:
    bra poll
check_stage:
    cmp a, #$03
    bne stage_or_effects
    mov $1f, #$00
    mov $15, #$00
    mov $48, #$00
    mov $58, #$00
    mov $68, #$00
    mov $f2, #$47
    mov $f3, #$00
    mov $f2, #$37
    mov $f3, #$00
    mov $18, #$00
    mov $14, #$00
    mov $f2, #$67
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov a, $10
    mov $f4, a
    bra return_top
stage_or_effects:
    cmp a, #$02
    bne effects
    mov a, $f5
    mov $13, a
    mov a, $f6
    mov $1e, a
    mov a, $10
    mov $f4, a
return_upper:
    bra return_top
effects:
    mov $11, #$00
    mov $12, #$00
; Independent pitches: 0800, 0C00, 1000, 1800; low bytes remain zero.
    mov a, $13
    and a, #$03
    mov x, a
    mov a, $d0+x
    mov $21, a
    mov $f2, #$63
    mov $f3, a
    mov a, $13
    xcn a
    and a, #$03
    mov x, a
    mov a, $d0+x
    mov $22, a
    mov $f2, #$53
    mov $f3, a
; A volume 3 requests global fade-out and retains A's previous voice level.
    mov a, $13
    lsr a
    lsr a
    and a, #$03
    mov x, a
    cmp x, #$03
    beq mute
    mov a, $d4+x
    mov $1a, a
    mov $17, #$60
    bra volume_a
mute:
    mov $17, #$00
volume_a:
    mov a, $1a
    mov $f2, #$60
    mov $f3, a
    mov $f2, #$61
    mov $f3, a
    mov a, $13
    xcn a
    lsr a
    lsr a
    and a, #$03
    mov x, a
    mov a, $d4+x
    mov $f2, #$50
    mov $f3, a
    mov $f2, #$51
    mov $f3, a
    bra effect_a
return_middle:
    bra return_upper
effect_a:
    mov a, $f5
    beq retrigger_a
    bmi stop_a
    mov $15, a
    bra start_a
retrigger_a:
    mov a, $15
    beq effect_b
start_a:
    mov x, a
    mov a, $d7+x
    mov $f2, #$64
    mov $f3, a
    mov a, $21
    mov $26, a
    mov $14, #$60
    mov $f2, #$67
    mov $f3, #$60
    mov $11, #$40
    bra effect_b
stop_a:
    mov $15, #$00
    mov $14, #$00
    mov $12, #$40
    bra effect_b
return_lower:
    bra return_middle
effect_b:
    mov a, $f6
    beq retrigger_b
    bmi stop_b
    mov $18, a
    bra start_b
retrigger_b:
    mov a, $18
    beq apply
start_b:
    mov x, a
    mov a, $e0+x
    mov $f2, #$54
    mov $f3, a
    mov $27, #$00
    mov $f2, #$57
    mov $f3, #$60
    mov a, $11
    or a, #$20
    mov $11, a
    bra apply
stop_b:
    mov $18, #$00
    mov a, $12
    or a, #$20
    mov $12, a
apply:
    mov $f2, #$5c
    mov a, $12
    or a, $48
    mov $f3, a
    mov $f2, #$4c
    mov a, $11
    mov $f3, a
    mov a, $1e
    mov $1b, a
    mov $1e, #$00
    cmp a, #$00
    beq effect_echo
    call $0700
effect_echo:
    mov a, $10
    mov $f4, a
    bra return_lower
