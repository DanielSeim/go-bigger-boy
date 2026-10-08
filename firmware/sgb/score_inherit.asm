; SPDX-License-Identifier: GPL-3.0-or-later
; Execute cached mix controls, rather than carrying unplayed parser tails.
; $AC/AD channel-2/3 pan, $AE/AF track volume; $B0 parser trailing-control flag.
; $FF cache fields mean the track has not supplied that control yet.
; Rehearsal validates inherited combinations before live DSP writes.
inherit_begin:
    mov $ac, #$0a
    mov $ad, #$0a
    mov $ae, #$7f
    mov $af, #$7f
    ret
inherit_event:
    mov a, $23
    cmp a, #$02
    bne inherit_voice3
    mov a, $8b
    cmp a, #$ff
    beq inherit_pan2
    mov $ac, a
inherit_pan2:
    mov a, $8c
    cmp a, #$ff
    beq inherit_track2
    mov $ae, a
inherit_track2:
    mov a, $ac
    mov $81, a
    mov a, $ae
    bra inherit_select
inherit_voice3:
    mov a, $8b
    cmp a, #$ff
    beq inherit_pan3
    mov $ad, a
inherit_pan3:
    mov a, $8c
    cmp a, #$ff
    beq inherit_track3
    mov $af, a
inherit_track3:
    mov a, $ad
    mov $81, a
    mov a, $af
inherit_select:
    mov $82, a
    mov $8c, a
    mov a, $81
    mov $8b, a
    call mix_validate
    mov a, $85
    mov $89, a
    mov a, $86
    mov $8a, a
    ret
