; SPDX-License-Identifier: GPL-3.0-or-later
; D4-only: appended after the frozen D3 helper. The host preflights complete
; ordered coverage of $2B00..32FF and $5000..50BF before ownership release.
; No old score/sample bytes or readiness survive a committed replacement.
; Archive $0504: 0 clearing, A4 cleared/unpublished, A5 all roots admitted.
atomic_upload_begin:
    call bridge_save_observations
    call bridge_mute
    mov $d1, #$00
    mov $d2, #$00
    mov $d8, #$00
    mov $db, #$00
    mov $dc, #$00
    mov a, #$00
    .byte $c5, $04, $05
    .byte $cd, $00
atomic_clear_score:
    .byte $d5, $00, $2b
    .byte $d5, $00, $2c
    .byte $d5, $00, $2d
    .byte $d5, $00, $2e
    .byte $d5, $00, $2f
    .byte $d5, $00, $30
    .byte $d5, $00, $31
    .byte $d5, $00, $32
    .byte $3d
    cmp x, #$00
    bne atomic_clear_score
atomic_clear_assets:
    .byte $d5, $00, $50
    .byte $3d
    cmp x, #$c0
    bne atomic_clear_assets
    mov a, #$a4
    .byte $c5, $04, $05
    ret
atomic_publish:
    mov $d2, #$01
    mov a, #$a5
    .byte $c5, $04, $05
    ret
