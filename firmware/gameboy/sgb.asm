; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB SGB header bootstrap. No proprietary instructions or artwork.
; Packet framing follows Pan Docs; packet layout checked by opaque execution.
IF !DEF(GBB_HANDOFF_A)
    DEF GBB_HANDOFF_A EQU $01
ENDC
SECTION "SGB bootstrap", ROM0[$0000]
SgbBoot:
    ld sp, $FFFE
    ld a, $30
    ldh [$FF00], a
    ld hl, $8000
    ld d, 32
    xor a
.page
    ld b, 0
.clear
    ld [hli], a
    dec b
    jr nz, .clear
    dec d
    jr nz, .page
    ; Original observable initialization boundary, without logo rendering.
    ld bc, 1146
.settle
    dec bc
    ld a, b
    or c
    jr nz, .settle
    nop
    nop
    nop
    ld hl, $FF25
    ld c, $11
    ld b, $77
    ld a, $80
    ldh [$FF26], a
    ldh [c], a
    inc c
    ld a, $F3
    ldh [c], a
    ld [hl], a
    ld a, b
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
    ; Keep LCD enable at the measured boundary; no proprietary VRAM assets.
    ld bc, 1209
.header_settle
    dec bc
    ld a, b
    or c
    jr nz, .header_settle
    nop
    nop
    nop
    nop
    nop
    ld a, $91
    ldh [$FF40], a
    ld hl, $C000
    ld c, 0
    nop
.send_packet
    xor a
    ldh [c], a
    ld a, $30
    ldh [c], a
    ld b, 16
.byte
    ld a, [hli]
    ld d, a
    ld e, 8
.bit
    rrc d
    ld a, $10
    jr c, .one
    ld a, $20
.one
    ldh [c], a
    ld a, $30
    ldh [c], a
    nop
    nop
    dec e
    jr nz, .bit
    dec b
    jr nz, .byte
    ld a, $20
    ldh [c], a
    ld a, $30
    ldh [c], a
    ; Four VBlank observations, separated by a fixed 4,092-clock idle.
    ; The reference polls LY every 32 clocks and does not poll line zero.
    nop
    nop
    nop
    nop
    nop
    nop
    ld b, 4
.vblank
    ldh a, [$FF44]
    cp 144
    jr nz, .vblank
    ld d, 0
.vblank_idle
    dec d
    jr nz, .vblank_idle
    dec b
    jr nz, .vblank
    ld a, l
    cp $60
    jr z, .finish
    nop
    nop
    nop
    jr .send_packet
.finish
    nop
    ld c, $14
    ld a, $C1
    or a
    ldh [$FF13], a
    ld a, $07
    ldh [$FF14], a
    ld a, GBB_HANDOFF_A
    jr .unmap
    ASSERT @ <= $00FE
    ds $00FE - @, 0
.unmap
    ldh [$FF50], a
    ASSERT @ == $0100
