; SPDX-License-Identifier: GPL-3.0-or-later
; CD-only instrument 2 mapping. Uploaded 32-byte object at $5000:
; four descriptor fields, four zero bytes, source-2 DIR entry, four zero
; bytes, one looping filter-0 BRR block, seven zero bytes.
; Fixed start/loop $5010 and header $b3 bound every sample fetch to nine bytes.
; Compare all structural bytes before any song can render. Sample nibbles are
; opaque audio data; no proprietary descriptors or samples are included.
.org $1800
uploaded_timing_init:
    .byte $cd, $00
uploaded_validate:
    mov a, $5000+x
    mov $df, a
    mov a, uploaded_structure+x
    cmp a, $df
    bne uploaded_bad
    .byte $3d
    mov a, x
    cmp a, #$11
    bne uploaded_validate
    .byte $cd, $19
uploaded_padding:
    mov a, $5000+x
    bne uploaded_bad
    .byte $3d
    mov a, x
    cmp a, #$20
    bne uploaded_padding
    call timing_init
    ret
uploaded_bad:
    jmp bridge_bad
uploaded_setup:
    call poly_setup
    mov $f2, #$5d
    mov $f3, #$50
    ret
uploaded_structure:
    .byte $02, $8f, $6f, $b8, $00, $00, $00, $00
    .byte $10, $50, $10, $50, $00, $00, $00, $00, $b3
