; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB SGB header bootstrap. No proprietary instructions or artwork.
; Packet framing follows Pan Docs; packet layout checked by opaque execution.
IF !DEF(GBB_HANDOFF_A)
    DEF GBB_HANDOFF_A EQU $01
ENDC
SECTION "SGB bootstrap", ROM0[$0000]
SgbBoot:
    di
    ld sp, $FFFE
    xor a
    ldh [$FF40], a
    ldh [$FF26], a
    ldh [$FFFF], a
    ldh [$FF0F], a
    ld hl, $8000
    ld bc, $2000
.clear
    xor a
    ld [hli], a
    dec bc
    ld a, b
    or c
    jr nz, .clear
    ld a, $80
    ldh [$FF26], a
    ldh [$FF11], a
    ld a, $F3
    ldh [$FF12], a
    ldh [$FF25], a
    ld a, $77
    ldh [$FF24], a
    ld a, $FC
    ldh [$FF47], a
    ; Six independent packets: ID, modulo-256 payload sum, 14 header bytes.
    ; Last packet contains six header bytes and eight zero padding bytes.
    ld hl, $C000
    ld de, $0104
    ld c, $F1
.packet
    ld a, c
    ld [hli], a
    inc hl
    push bc
    ld b, 14
    ld c, 0
.payload
    ld a, e
    cp $50
    jr nc, .padding
    ld a, [de]
    jr .store
.padding
    xor a
.store
    inc de
    ld [hli], a
    add c
    ld c, a
    dec b
    jr nz, .payload
    ld a, l
    sub 15
    ld l, a
    ld [hl], c
    add 15
    ld l, a
    pop bc
    inc c
    inc c
    ld a, c
    cp $FD
    jr nz, .packet
    ld a, $91
    ldh [$FF40], a
    ld hl, $C000
    ld d, 6
    ld a, $30
    ldh [$FF00], a
    ; Idle-high guard before the first reset pulse (at least 15 M-cycles).
    nop
    nop
    nop
    nop
    nop
    nop
.send_packet
    xor a
    call .pulse
    ld e, 16
.byte
    ld a, [hli]
    ld c, a
    ld b, 8
.bit
    rrc c
    ld a, $10
    jr c, .one
    ld a, $20
.one
    call .pulse
    dec b
    jr nz, .bit
    dec e
    jr nz, .byte
    ld a, $20
    call .pulse
    ; Four complete VBlank-to-line-zero intervals, including after packet 6.
    ld b, 4
.vblank
    ldh a, [$FF44]
    cp 144
    jr nz, .vblank
.line_zero
    ldh a, [$FF44]
    and a
    jr nz, .line_zero
    dec b
    jr nz, .vblank
    dec d
    jr nz, .send_packet
    ld a, $C1
    ldh [$FF13], a
    ld a, $07
    ldh [$FF14], a
    ld bc, $0014
    ld de, 0
    xor a
    ld a, GBB_HANDOFF_A
    or a
    jp $00FE
.pulse
    ldh [$FF00], a
    nop
    nop
    ld a, $30
    ldh [$FF00], a
    ; With CALL/RET and caller work, high spaces exceed 15 M-cycles.
    nop
    nop
    nop
    nop
    ret
    ASSERT @ <= $00FE
    ds $00FE - @, 0
    ldh [$FF50], a
    ASSERT @ == $0100
