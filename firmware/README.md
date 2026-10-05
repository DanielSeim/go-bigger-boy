# Original replacement firmware

This directory contains GBB's original firmware sources and generated images.
It does **not** contain Nintendo boot ROMs, disassemblies, logos, sound assets,
SNES program ROMs, or SPC700 IPL dumps. Sources and generated images use the
repository's GPL-3.0-or-later license.

## DMG revision 4

`gameboy/dmg.asm` is an original, 256-byte **fast cold-start** implementation.
It is opt-in and not a cycle-exact recreation of Nintendo's startup.
Instant startup remains the default; the existing diagnostic ROM is unchanged.

The firmware executes on the emulated CPU from `0000`, with the LCD and APU
off and the divider starting at zero. It establishes the stack, disables
interrupts, clears all 8 KiB of VRAM, validates the header checksum, initializes
the visible DMG sound/display/input/timer/serial registers, and lets channel 1's
envelope decay with the mixer disconnected. It then reconnects the mixer,
settles the DC transient, enables the LCD, waits for LY=0 after the first
rendered frame, and establishes the documented CPU handoff state. Startup takes
about 1.03 emulated seconds; it deliberately omits the original animation/chime.
CPU-written DIV reset and bounded delay loops establish divider `ABC8` and
internal LCD line 153, dot 396 (visible LY=0, STAT=`85`), for either checksum path.
Revision 3 also aligns the inherited APU sequencer and silent CH1 waveform
state through ordinary writes/delays, fixing measured Pokémon audio differences
without changing the readable register contract. See
[cartridge audio validation](../docs/dmg-boot-audio-validation.md) for commands,
coverage and limitations.
Revision 4 aligns the idle serial phase with opaque cold-reference execution
using an earlier ordinary SC clear, while preserving subsequent initialization
timing. It also adds first-transfer/link/restore validation; see
[serial validation](../docs/dmg-boot-serial-validation.md).
The `LDH [FF50],A` at `00FE` unmaps the image; the next instruction is fetched
from the cartridge at `0100`. The checksum determines whether F is `80` or `B0`.
An invalid checksum keeps execution inside the boot ROM with the LCD off.

No Nintendo-logo authentication is performed: a homebrew cartridge with a
correct checksum does not need that logo to boot through this replacement.
No boot logo or trademark tile is installed in VRAM. WRAM and cartridge RAM
are not modified by the firmware. Its stack uses two bytes at `FFFC`–`FFFD`;
it does not write the diagnostic `GBB` HRAM marker.

### Game Boy Pocket (MGB)

The MGB image is built from the same original source with `GBB_HANDOFF_A=$FF`.
It differs from the DMG image by exactly one byte: the immediate in the final
`LD A` instruction. Both execute the same instructions for the same number of
clocks; Pocket hands the cartridge `A=FF` rather than `01`, including the
`FF50` unmap write. This follows the documented [Pocket handoff contract](https://gbdev.io/pandocs/Power_Up_Sequence.html).
Both checksum-dependent flag paths, peripheral initialization, quiet audio,
reset and mapped-image save-state behavior are retained. The optional original
GBB animation/chime is also available on MGB.

Tests use synthetic logo-free cartridges and verify image differences, CPU
registers, timer/LCD/APU state, cartridge-visible detection, checksum rejection,
reset, mid-boot restore and frontend core selection. They do not establish
cycle-exact equivalence to physical Pocket cold startup or its original ROM.

### Rebuild and verify

Normal builds consume the checked-in fast and animated `*_boot_image.hpp`
images for DMG and MGB; they do not
require RGBDS, Python, downloaded ROMs, or network access. With RGBDS (`rgbasm`
and `rgblink`; tested with 1.0.1) and Python installed:

```sh
python3 scripts/build_dmg_boot_rom.py
python3 scripts/build_dmg_boot_rom.py --check
python3 scripts/build_dmg_boot_rom.py --check-source # provenance check, no RGBDS required
python3 scripts/build_dmg_boot_rom.py --check --output /tmp/gbb-dmg-boot.bin
python3 scripts/build_dmg_boot_rom.py --model mgb
python3 scripts/build_dmg_boot_rom.py --model mgb --check
python3 scripts/build_dmg_boot_rom.py --model mgb --check-source
python3 scripts/build_dmg_boot_rom.py --model mgb --check --output /tmp/gbb-mgb-boot.bin
python3 scripts/build_dmg_boot_rom.py --animated --check
python3 scripts/build_dmg_boot_rom.py --model mgb --animated --check
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
core API (also named `replacement_mgb` for Pocket). The selected DMG/MGB
hardware profile chooses its matching image; other profiles are rejected by
the raw emulator API rather than silently receiving monochrome initialization.
Desktop, Android and web settings expose **Startup** with
**Instant startup** (default), **GBB fast boot**, and **GBB animated boot**. Desktop and Android
persist `boot.Startup = instant`, `replacement-dmg`, or `animated-dmg` in `settings.ini`; web
persists the choice in browser local storage. No external firmware download is
required: the image is bundled in every build.

The existing `replacement-dmg` and `animated-dmg` setting IDs are retained for
compatibility; they select the matching firmware on both DMG and MGB.

The animated option selects a separate original 256-byte firmware variant. Its
CPU-executed wait adds exactly 19,136,512 clocks (292 whole DIV wraps), making
startup about **5.59 seconds**, while preserving the fast boot's eventual
CPU/register/timer/LCD/APU handoff apart from the elapsed clock. Fast mode stays
about 1.03 seconds. No gameplay runs behind the intro.

The shared presentation keeps original descending **Go Bigger Boy** lettering,
moving one pixel on alternating 3/2-VBlank waits, then holding. The chime is
synthesized by a separate GBB APU, using the observable DMG/MGB duty, envelope,
period and trigger timings: approximately 1048.576 Hz followed by 2080.508 Hz,
about 84 ms apart, with the hardware's stepped decay. It is cached before
playback so generation cannot stall the first note. An execution-only local
reference measured the first triggers at clocks 17,467,324 and 17,818,480 and
original handoff at 23,440,324; replacement handoff is within one video frame.
No Nintendo logo, recorded audio, ROM instructions or extracted graphics are
included. This aligns presentation behavior, not physical-speaker frequency
response or cycle-exact original firmware execution.

**A or Start** hides the presentation and chime, not the firmware wait or header
validation; use fast/instant mode to avoid the wait. The startup screen stays
flat even with voxel rendering selected, and the chosen renderer resumes at
cartridge handoff. Muted audio stays muted.

The choice applies when a ROM is started again (including frontend restart),
not to an already running core. It does not force a hardware model. Automatic
selection still prefers CGB for CGB-capable titles and SGB for SGB-capable titles;
select the **DMG** hardware profile explicitly if you want to use the replacement
with a dual-mode title, or **MGB** for Pocket. Other profiles retain instant startup. Experimental
SNES-side SGB firmware playback remains separate and still needs user firmware.

### State and reset contracts

GBB uses a deterministic zero-filled power-on RAM baseline, not an assertion
about random physical RAM contents. Reset runs the replacement again from that
baseline, retains cartridge/mapper/battery data, display palette and host audio preference,
and cancels pending DMA/timer/link activity. It is a firmware restart, **not** a
physical cartridge power cycle or mapper reset.

Save-state version 40 includes the actual 256-byte mapped boot image when a
boot is in progress, so a mid-boot restore does not depend on the destination
constructor having selected the same firmware. Completed boots have no image
payload. Version 42 additionally stores animation/skip progress and the chime
cursor, so a mid-intro restore continues deterministically. Versions 1–41 remain
loadable using their existing compatibility paths, without adding an intro to
an older state. Reset follows the receiving emulator's configured startup mode.
Reset after a restore still follows the receiving emulator's configured boot
mode; restoring a state does not silently change its startup preference.

### Provenance and remaining work

Local execution checks passed the synthetic cold-state/handoff/reset/state
contracts and the v7.0 test bundle's Mooneye `boot_regs-dmgABC`,
`boot_div-dmgABCmgb`, and `boot_hwio-dmgABCmgb`, plus Blargg
`cpu_instrs`, `instr_timing`, and `mem_timing`, all with `--model dmg --dmg-boot`.
Those external tests are behavioral validation, not firmware build inputs.
They do not establish equivalence of every power-on or title-level behavior.

The implementation uses the public [Pan Docs power-up hardware contract](https://gbdev.io/pandocs/Power_Up_Sequence.html).
No proprietary ROM or disassembly was read to produce its instructions or data.
The downloaded originals in Git-ignored `roms/` are **not** inputs to the build
or automated execution contracts, and are not included in releases. Automated
tests use synthetic homebrew headers with no Nintendo logo. A separate opt-in
[black-box comparison harness](../docs/dmg-boot-validation.md) can execute a
user-provided local original as an opaque behavioral reference; it never
disassembles, exports or incorporates its instructions into the replacement.

This first milestone is hardware initialization and deterministic handoff, not
100% behavioral equivalence. Fast mode deliberately shortens startup; animated
mode now follows the measured scroll cadence and synthesized chime parameters.
Cold DIV/resampler phase, original logo tiles/tile maps and trademark graphics
still differ from the original. Games or power-up conformance tests that depend on those
details may fail; the existing post-boot path remains the production default.
The core now models DMG's early LY wrap on the final VBlank line, so fast-start
enters the cartridge with LY=0 and STAT=`85` (VBlank), rather than erroneously
waiting until pixel transfer. Local same-core reference runs match the stable
CPU/I/O/RAM contract for Pokémon Blue, Super Mario Land, Tetris and Donkey Kong,
with matching framebuffer hashes after a short followup run. They do not
prove physical hardware equivalence; the cold original's DIV phase is four clocks
behind the hardware-test-backed fast-start phase, and resampler phase
still differs. Serial phase now matches that cold reference, but the unchanged
production post-boot profile and attached-cable policy remain distinct.
The firmware hands off with channel 1's envelope at
zero while retaining the canonical readable sound registers and an active DAC.
Logo-free cartridges that never write the APU stay below -72 dBFS in the
automated one-second quiet-output check; retriggering produces normal sound.
In the fast replacement mode there is a DAC/mixer power-on DC transient during
boot, not a chime. That mode uses no host mute or private post-boot state
injection. The optional animated mode replaces only the returned pre-handoff
PCM with its separately synthesized chime, without changing the game APU state.
Broader title-level and independent-hardware validation and model-specific boot
implementations remain future milestones. SNES-side
SGB1/SGB2 and SPC700 replacement firmware are not implemented here yet.
