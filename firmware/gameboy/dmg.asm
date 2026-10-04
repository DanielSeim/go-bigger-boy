; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB DMG cold-start firmware, revision 1.
; Written from the public hardware contract, not a Nintendo disassembly.
; Intentionally no Nintendo logo, trademark tile, animation or logo check.

SECTION "DMG startup", ROM0[$0000]
DmgBoot:
    di
    ld sp, $FFFE
    xor a
    ldh [$FF40], a                ; LCD off while VRAM is initialized
    ldh [$FF26], a                ; reset APU registers/channels
    ld [$FFFF], a                 ; interrupts remain disabled
    ldh [$FF00], a                ; select both JOYP groups
    ldh [$FF01], a
    ldh [$FF02], a
    ldh [$FF05], a
    ldh [$FF06], a
    ldh [$FF07], a
    ldh [$FF0F], a
    ldh [$FF42], a
    ldh [$FF43], a
    ldh [$FF45], a
    ldh [$FF4A], a
    ldh [$FF4B], a

    ; No assumptions about the contents of video RAM at power-on.
    ld hl, $8000
    ld bc, $2000
.clear_vram
    ld [hli], a
    dec bc
    ld a, b
    or c
    jr z, .vram_ready
    xor a
    jr .clear_vram
.vram_ready

    ; Validate the cartridge checksum without storing a proprietary logo.
    ld hl, $0134
    ld b, $19
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
    jr nz, .invalid_header

    ; Initialize sound using CPU-visible writes, not a post-boot snapshot.
    ld hl, .io_values
    ld b, (.io_end - .io_values) / 2
.init_io
    ld a, [hli]
    ld c, a
    ld a, [hli]
    ldh [c], a
    dec b
    jr nz, .init_io
    ld a, $91
    ldh [$FF40], a

    ; Let the LCD produce one complete frame before entering the cartridge.
.wait_vblank
    ldh a, [$FF44]
    cp $90
    jr c, .wait_vblank
.wait_line_zero
    ldh a, [$FF44]
    and a
    jr nz, .wait_line_zero

    ; The documented DMG flags depend on the header checksum being zero.
    ld a, [$014D]
    and a
    ld bc, $0080
    jr z, .flags_ready
    ld bc, $00B0
.flags_ready
    push bc
    pop af
    ld a, $01
    ld bc, $0013
    ld de, $00D8
    ld hl, $014D
    jp $00FE
.invalid_header
    jr .invalid_header

.io_values
    db $26, $80, $10, $80, $11, $80, $12, $F3
    db $13, $00, $14, $80, $1C, $80, $24, $77, $25, $F3
    db $47, $FC, $48, $FF, $49, $FF
.io_end
    ASSERT @ <= $00FE, "DMG startup must fit below the unmap instruction"
    ds $00FE - @, 0
    ldh [$FF50], a                ; unmap at FE; next fetch is cartridge 0100
    ASSERT @ == $0100, "DMG boot image must be exactly 256 bytes"
