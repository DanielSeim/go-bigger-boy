; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB DMG cold-start firmware, revision 4.
; Written from the public hardware contract, not a Nintendo disassembly.
; Intentionally no Nintendo logo, trademark tile, animation or logo check.
; The MGB build changes only the final accumulator immediate to $FF.
IF !DEF(GBB_HANDOFF_A)
    DEF GBB_HANDOFF_A EQU $01
ENDC
ASSERT GBB_HANDOFF_A == $01 || GBB_HANDOFF_A == $FF

SECTION "DMG startup", ROM0[$0000]
DmgBoot:
    di
IF DEF(GBB_ANIMATED)
    ; Original delay code, not extracted from any reference firmware.
    ; Exactly 292 DIV wraps (19,136,512 clocks = 4.5625 seconds).
    ; Keeping a whole number of wraps preserves the fast firmware's later
    ; DIV/APU/serial phase. The shared host presentation runs during this wait.
    ld d, 12
.intro_outer
    ld bc, 56952
.intro_inner
    dec bc
    ld a, b
    or c
    jr nz, .intro_inner
    dec d
    jr nz, .intro_outer
    ld b, 21
.intro_tail
    dec b
    jr nz, .intro_tail
    nop
    nop
ENDC
    ld sp, $FFFE
    xor a
    ; Clear SC early so the remaining ordinary CPU clocks leave serial phase
    ; 452 at handoff, as observed on the unchanged opaque cold reference.
    ; The NOP and 12-clock LDH IE below preserve the original 96-clock prefix:
    ; later APU, DIV and LCD initialization timings are unchanged.
    nop
    ldh [$FF02], a
    ldh [$FF40], a                ; LCD off while VRAM is initialized
    ldh [$FF26], a                ; reset APU registers/channels
    ldh [$FFFF], a                ; interrupts remain disabled
    ldh [$FF00], a                ; select both JOYP groups
    ldh [$FF01], a
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
    jp nz, .invalid_header

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

    ; Keep the mixer disconnected while the canonical F3 envelope decays.
    ; Two 65535-iteration loops take over 48 envelope clock ticks (64 Hz).
    ; This uses normal APU clocks, not model-dependent envelope-write quirks.
    ld d, 2
.settle_envelope
    ld bc, $FFFF
.settle_loop
    dec bc
    ld a, b
    or c
    jr nz, .settle_loop
    dec d
    jr nz, .settle_envelope
    ; Match the observed silent CH1 waveform handoff (step 2, timer 30).
    ; 28*12+8+8 = 352 clocks, before the final DIV phase is established.
    ld bc, 12
.waveform_phase
    dec bc
    ld a, b
    or c
    jr nz, .waveform_phase
    nop
    nop
    ld a, $F3
    ldh [$FF25], a

    ; Establish a repeatable fast-start divider phase using only CPU writes.
    ; Leave time for the mixer high-pass filter and a completed LCD frame.
    xor a
    ldh [$FF04], a
    ; Align the inherited envelope/length sequencer without resetting the APU.
    ; Four forced falling edges advance step 5 to step 1. Each wait observes
    ; the public DIV-APU bit; no hidden state or envelope-write quirk is used.
    ld b, 4
.apu_phase
    ldh a, [$FF04]
    and $10
    jr z, .apu_phase
    xor a
    ldh [$FF04], a
    dec b
    jr nz, .apu_phase
    ldh [$FF04], a                ; final reset is low: no extra APU edge
    ; 6085 iterations: 28*N+8 clocks including LD BC, then four NOPs.
    ; The LCD poll/phase-alignment/handoff path takes 70180 clocks.
    ; (170388 + 16 + 70180) modulo 65536 = ABC8.
    ; The extra two divider wraps settle the analog DC transient before entry.
    ld bc, 6085
.divider_delay
    dec bc
    ld a, b
    or c
    jr nz, .divider_delay
    nop
    nop
    nop
    nop
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

    ; Finish on internal line 153, dot 396 (LY=0, mode 1).
    ld b, 11
.lcd_phase
    dec b
    jr nz, .lcd_phase
    nop
    nop

    ; The documented DMG flags depend on the header checksum being zero.
    ld a, [$014D]
    and a
    ld bc, $0080
    jr nz, .nonzero_checksum
    nop
    nop
    nop
    nop
    jr .flags_ready
.nonzero_checksum
    ld bc, $00B0
    jr .flags_ready
.flags_ready
    push bc
    pop af
    ld a, GBB_HANDOFF_A
    ld bc, $0013
    ld de, $00D8
    ld hl, $014D
    jp $00FE
.invalid_header
    jr .invalid_header

.io_values
    db $26, $80, $10, $80, $11, $80, $12, $F3
    ; Opaque reference execution leaves CH1's period at 252 clocks (7C1).
    ; A second ordinary trigger establishes its inherited low-two-bit phase.
    db $13, $C1, $14, $87, $14, $87, $1C, $80, $24, $77, $25, $00
    db $47, $FC, $48, $FF, $49, $FF
.io_end
    ASSERT @ <= $00FE, "DMG startup must fit below the unmap instruction"
    ds $00FE - @, 0
    ldh [$FF50], a                ; unmap at FE; next fetch is cartridge 0100
    ASSERT @ == $0100, "DMG boot image must be exactly 256 bytes"
