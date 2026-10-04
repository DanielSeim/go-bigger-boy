# Original replacement firmware

This directory contains GBB's original firmware sources and generated images.
It does **not** contain Nintendo boot ROMs, disassemblies, logos, sound assets,
SNES program ROMs, or SPC700 IPL dumps. Sources and generated images use the
repository's GPL-3.0-or-later license.

## DMG revision 1

`gameboy/dmg.asm` is an original, 256-byte **fast cold-start** implementation.
It is opt-in, DMG-only, and not a cycle-exact recreation of Nintendo's startup.
The production post-boot path and existing diagnostic ROM are unchanged.

The firmware executes on the emulated CPU from `0000`, with the LCD and APU
off and the divider starting at zero. It establishes the stack, disables
interrupts, clears all 8 KiB of VRAM, validates the header checksum, initializes
the visible DMG sound/display/input/timer/serial registers, enables the LCD,
waits for LY=0 after the first rendered frame, and establishes the documented CPU handoff state.
The `LDH [FF50],A` at `00FE` unmaps the image; the next instruction is fetched
from the cartridge at `0100`. The checksum determines whether F is `80` or `B0`.
An invalid checksum keeps execution inside the boot ROM with the LCD off.

No Nintendo-logo authentication is performed: a homebrew cartridge with a
correct checksum does not need that logo to boot through this replacement.
No boot logo or trademark tile is installed in VRAM. WRAM and cartridge RAM
are not modified by the firmware. Its stack uses two bytes at `FFFC`–`FFFD`;
it does not write the diagnostic `GBB` HRAM marker.

### Rebuild and verify

Normal builds consume the checked-in `gameboy/dmg_boot_image.hpp`; they do not
require RGBDS, Python, downloaded ROMs, or network access. With RGBDS (`rgbasm`
and `rgblink`; tested with 1.0.1) and Python installed:

```sh
python3 scripts/build_dmg_boot_rom.py
python3 scripts/build_dmg_boot_rom.py --check
python3 scripts/build_dmg_boot_rom.py --check-source # provenance check, no RGBDS required
python3 scripts/build_dmg_boot_rom.py --check --output /tmp/gbb-dmg-boot.bin
```

The generator assembles only `dmg.asm`, verifies the 256-byte size and final
unmap instruction, and records source and image SHA-256 hashes in the generated header.
It refuses to overwrite an existing `--output` image. CTest checks source/image
reproducibility when RGBDS and Python are available, and checks source/image
provenance whenever Python is available; the execution contracts
always run when core tests are enabled.

```sh
build/gbb_test_runner /path/to/homebrew.gb --model dmg --dmg-boot --protocol serial
ctest --test-dir build -R 'gameboy_dmg_' --output-on-failure
```

The firmware is also available through `BootRomMode::replacement_dmg` in the
core API. Other hardware profiles are rejected rather than silently receiving
DMG initialization. There is not yet a desktop, Android, or web settings toggle.

### State and reset contracts

GBB uses a deterministic zero-filled power-on RAM baseline, not an assertion
about random physical RAM contents. Reset runs the replacement again from that
baseline, retains cartridge/mapper/battery data, display palette and host audio preference,
and cancels pending DMA/timer/link activity. It is a firmware restart, **not** a
physical cartridge power cycle or mapper reset.

Save-state version 40 includes the actual 256-byte mapped boot image when a
boot is in progress, so a mid-boot restore does not depend on the destination
constructor having selected the same firmware. Completed boots have no image
payload. Versions 1–39 remain loadable using their existing compatibility path.
Reset after a restore still follows the receiving emulator's configured boot
mode; restoring a state does not silently change its startup preference.

### Provenance and remaining work

Local execution checks passed the synthetic cold-state/handoff/reset/state
contracts and the v7.0 test bundle's Mooneye `boot_regs-dmgABC` plus Blargg
`cpu_instrs`, `instr_timing`, and `mem_timing`, all with `--model dmg --dmg-boot`.
Those external tests are behavioral validation, not firmware build inputs.
They do not establish equivalence of every power-on or title-level behavior.

The implementation uses the public [Pan Docs power-up hardware contract](https://gbdev.io/pandocs/Power_Up_Sequence.html).
No proprietary ROM or disassembly was read to produce its instructions or data.
The downloaded originals in Git-ignored `roms/` were **not** inputs to the build
or execution contracts, and are not included in releases. Tests use synthetic
homebrew headers with no Nintendo logo.

This first milestone is hardware initialization and deterministic handoff, not
100% behavioral equivalence. In particular, startup duration, DIV/serial/APU
phase, logo tiles/tile maps, trademark graphics, animation and chime differ
from the original. Games or power-up conformance tests that depend on those
details may fail; the existing post-boot path remains the production default.
The fast-start path currently enters the cartridge with LY=0 and STAT=`87`
(pixel transfer), not the original DMG's STAT=`85`; this difference is tested
explicitly rather than described as hardware-equivalent startup timing.
An original GBB splash/chime, title-level and independent-reference validation,
and model-specific boot implementations remain future milestones. SNES-side
SGB1/SGB2 and SPC700 replacement firmware are not implemented here yet.
