; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB CGB startup firmware. No extracted instructions or assets.
; GBB supplies deterministic cold RAM and chooses cartridge compatibility mode;
; this firmware performs the CPU-visible initialization and handoff.
; Animated builds use the same host presentation mechanism as DMG/MGB.
SECTION "CGB startup", ROM0[$0000]
CgbBoot:
    di
    ld sp, $FFFE
    xor a
    ldh [$FFFF], a
    ldh [$FF40], a
    ldh [$FF26], a
    ldh [$FF04], a
    ld a, $30
    ldh [$FF00], a
    ; Clear both banks when the cartridge exposes CGB registers.
    ldh a, [$FF4F]
    cp $FF
    jr z, .bank_zero
    ld a, 1
    ldh [$FF4F], a
    call .clear_vram
.bank_zero
    xor a
    ldh [$FF4F], a
    call .clear_vram
    ; Checksum validation deliberately does not authenticate a logo.
    ld hl, $0134
    ld b, 25
    ld c, 0
.checksum
    ld a, c
    sub [hl]
    dec a
    ld c, a
    inc hl
    dec b
    jr nz, .checksum
    ld a, [hl]
    cp c
    jp nz, .failed
    ld hl, .io_values
    ld b, (.io_end - .io_values) / 2
.io
    ld a, [hli]
    ld c, a
    ld a, [hli]
    ldh [c], a
    dec b
    jr nz, .io
IF !DEF(GBB_CGB0)
    ; Later color models initialize wave RAM with alternating $00/$FF.
    ld c, $30
    xor a
.wave
    ldh [c], a
    cpl
    inc c
    bit 6, c
    jr z, .wave
ENDC
    ; White native BG/OBJ palettes; GB compatibility uses GBB's existing
    ; title-selected compatibility palette without altering that path.
    ld a, $80
    ldh [$FF68], a
    ldh [$FF6A], a
    ld b, 64
    ld a, $FF
.palettes
    ldh [$FF69], a
    ldh [$FF6B], a
    dec b
    jr nz, .palettes
    ; Allow a normally clocked pulse envelope to settle before handoff.
IF DEF(GBB_ANIMATED)
    ld d, 7
ELSE
    ld d, 2
ENDC
.wait_outer
    ld bc, $FFFF
.wait_inner
    dec bc
    ld a, b
    or c
    jr nz, .wait_inner
    dec d
    jr nz, .wait_outer
    ld a, $F3
    ldh [$FF25], a
    ld a, $91
    ldh [$FF40], a
    ; Native and compatibility games have different documented registers.
    ld bc, 0
    ld de, $0008
    ld hl, $007C
    ld a, [$0143]
    bit 7, a
    jr z, .compatibility
    ld de, $FF56
    ld hl, $000D
    jr .handoff
.compatibility
    ; Nintendo licensee title checksum is a hardware-observable register
    ; convention, not a logo check or copied palette table.
    ld a, [$014B]
    cp 1
    jr z, .title
    cp $33
    jr nz, .handoff
    ld a, [$0144]
    cp $30
    jr nz, .handoff
    ld a, [$0145]
    cp $31
    jr nz, .handoff
.title
    ld hl, $0134
    ld c, 16
.sum
    ld a, [hli]
    add b
    ld b, a
    dec c
    jr nz, .sum
    ld hl, $007C
    ld a, b
    cp $43
    jr z, .legacy_map
    cp $58
    jr nz, .handoff
.legacy_map
    ld hl, $991A
.handoff
    xor a
    ldh [$FF0F], a
    ld a, $11
    jp $00FE
.failed
    jr .failed
.clear_vram
    ld hl, $8000
    ld bc, $2000
    xor a
.clear
    ld [hli], a
    dec bc
    ld a, b
    or c
    ret z
    xor a
    jr .clear
.io_values
    db $26, $80, $11, $BF, $12, $F3, $13, $C1, $14, $87
    db $24, $77, $25, $00, $47, $FC, $48, $FF, $49, $FF
.io_end
    ASSERT @ <= $00FE
SECTION "CGB unmap", ROM0[$00FE]
    ldh [$FF50], a
