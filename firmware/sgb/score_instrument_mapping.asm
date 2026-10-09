; SPDX-License-Identifier: GPL-3.0-or-later
; D6-only. $5018/$5019 are distinct score IDs for owned DSP slots 2/3.
; Any byte ID, including 0/255, is valid when explicitly mapped.
; Admission caches resolved slots, never score IDs or uploaded addresses.
mapping_validate:
    mov a, $5018
    mov $df, a
    mov a, $5019
    cmp a, $df
    beq mapping_bad
    ret
mapping_lookup:
    mov $df, a
    mov a, $5018
    cmp a, $df
    beq mapping_slot2
    mov a, $5019
    cmp a, $df
    bne mapping_bad
    mov a, #$03
    ret
mapping_slot2:
    mov a, #$02
    ret
mapping_bad:
    jmp pair_reject
