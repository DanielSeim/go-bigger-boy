; SPDX-License-Identifier: GPL-3.0-or-later
; D4-only preflight. M=16, X=16. $52/$54 are exclusive coverage cursors.
; Per-object chunks must be contiguous/in order; the objects may interleave.
; Generic framing/wrap guards run first. Nothing uploads until final coverage.
atomic_block:
    lda $42
    cmp.w #$2b00
    bcc atomic_bad
    cmp.w #$3300
    bcs atomic_asset_block
    cmp $52
    bne atomic_bad
    clc
    adc $40
    cmp.w #$3301
    bcs atomic_bad
    sta $52
    rts
atomic_asset_block:
    cmp.w #$5000
    bcc atomic_bad
    cmp.w #$50c0
    bcs atomic_bad
    cmp $54
    bne atomic_bad
    clc
    adc $40
    cmp.w #$50c1
    bcs atomic_bad
    sta $54
    rts
atomic_complete:
    lda $42
    cmp.w #$0400
    bne atomic_bad
    lda $52
    cmp.w #$3300
    bne atomic_bad
    lda $54
    cmp.w #$50c0
    bne atomic_bad
    lda $42
    rts
atomic_bad:
    jmp invalid_list_wide
