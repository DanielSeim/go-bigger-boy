# Original SGB/SGB2 boot validation

This milestone replaces only the 256-byte Game Boy-side boot image. It does
not replace the SNES program ROM or SPC700 IPL, introduce a branded SNES intro,
or establish full cycle equivalence with original hardware. Source and generated
images are original GPL-3.0-or-later GBB code. Private inputs and captures remain
in ignored directories and are not release assets.

## Automated ROM-free contracts

`gameboy_sgb_replacement_boot` exercises both models and both replacement
startup preferences using synthetic headers, including invalid checksums and
zero/varied data. It checks all six packet IDs, payload sums, padding, pulse/space
widths, four-VBlank scheduling, CPU/JOYP/LCD handoff, deterministic reset, and
save/restore during a live packet. Diagnostics are deliberately reset on restore;
the serialized machine state and subsequent execution still match exactly.
The firmware adapter test covers missing GB-side images, bundled selection,
original-image overrides and malformed-override rejection. Existing DMG/CGB
startup behavior is checked separately. Python tool tests reject corrupt packet,
buffer, CPU and readable-I/O comparisons.

```sh
ctest --test-dir build-desktop --output-on-failure \
  -R 'gameboy_sgb_replacement_boot|gameboy_sgb_boot_validation_tool|gameboy_sgb[2]?_firmware_source|gameboy_sgb_firmware_core_contract'
```

## Optional opaque reference comparison

The existing boot probe now accepts `--model dmg|sgb|sgb2`; omitted model remains
DMG for backward compatibility. The comparison tool executes each private boot
image without disassembly, using the same cold baseline and cartridge bytes.
It compares CPU registers, the 96-byte header buffer, actual JOYP packets and
stable readable I/O. Reports contain hashes and timing metadata, not firmware,
logo graphics, packet contents or memory captures.

```sh
python3 scripts/validate_sgb_boot.py \
  --probe build-desktop/gbb_dmg_boot_probe --reference-dir roms \
  --rom 'roms/Donkey Kong (JU) (V1.1) [S][!].gb' \
  --report build-sgb-boot-validation/contract.json
```

`roms` must contain the caller's `sgb.boot.rom` and `sgb2.boot.rom`; four
logo-free synthetic header patterns are always included. Tetris, Donkey Kong and
Pokémon Blue plus those patterns passed all fourteen model/header contract comparisons locally.
The optional `scripts/sgb_boot_handoff_reference.c` can be compiled against a
separate local SameBoy core and run as `PROBE sgb|sgb2 GAME BOOT`. Independent
execution matched all handoff CPU registers for both originals and replacements.
The external core is validation-only and never bundled or linked into GBB.

## Limits of the evidence

The replacement deliberately leaves VRAM clear instead of reproducing Nintendo
logo tiles. Packet timings and exact DIV, LCD and analog audio phase are not
identical to the original, and the report explicitly sets `cycle_exact=false`.
The SameBoy Tetris check observed LY=0/STAT=85 for both boot images but different
DIV values. GBB's own SGB final-VBlank LY behavior differs from that independent
reference; this bootstrap does not change the existing PPU timing model to hide
that discrepancy. Matching packets and registers is not proof of every title's
sub-frame behavior or physical hardware equivalence.

Whole-host local Donkey Kong gameplay replays with private SGB1/SGB2 program and
SPC IPL images completed with the replacement boot: eleven input events, three
delivered sound commands, two audible commands, nonzero combined audio and over
5,000 GB frames on each model. These are integration checks, not an independent
audio-fidelity oracle. Original-override audio baselines remain separate tests.

A replacement-boot Pokémon Blue SGB2 new-game replay also completed over 1,500
GB frames and fourteen input events with nonzero combined audio. The existing
original-override title suite passed its exact audio hashes, including mid-stream
save/restore, for both SGB models. These checks do not resolve the phase limits
described above.
