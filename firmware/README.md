# Original replacement firmware

This directory contains GBB's original firmware sources and generated images.
It does **not** contain Nintendo boot ROMs, disassemblies, logos, sound assets,
proprietary SNES program ROMs, or SPC700 IPL dumps. Sources and generated images
use the repository's GPL-3.0-or-later license.

## Current startup and runtime scope (audited 2026-10-05)

Bundled fast/animated startup covers DMG0, DMG, MGB, CGB-0/later CGB,
AGB-0/AGB startup compatibility, SGB and SGB2. Core API names are
`BootRomMode::replacement` and `BootRomMode::animated`; old DMG-specific aliases
do not exist. Frontend setting IDs are `instant` (default), `replacement`, and
`animated`; unknown/old IDs fall back to instant. SGB's two preferences run
the same GB-side header bootstrap without a DMG splash. Automatic selection
never chooses AGB profiles.

Bundled startup needs no external boot firmware. Version 0.36.0's separate
experimental desktop SGB audio backend requires a caller-owned SGB1/SGB2
SNES program ROM. The original SPC700 IPL is bundled; an optional 64-byte
`spc700.rom` overrides it. An optional 256-byte GB boot override
replaces the bundled bootstrap. HLE remains the ordinary-launch default;
Android/web do not expose this backend. See
[runtime requirements](../docs/sgb-host.md#experimental-desktop-playback).

Ordinary GB save states are version 42 (mapped image from version 40, STAT
acknowledgment from 41, presentation state from 42). Whole firmware-host
snapshots instead use `GBBSHOST` version 3 inside the desktop `GBBFW001`
wrapper; host versions 1/2 are rejected. DSP/APU components remain version 1.
Historical validation counts/hashes in the linked research documents retain
their capture scope and are not new audit runs. Current SGB
[test pins](../docs/sgb-boot-validation.md#current-test-pins-and-provenance-audited-2026-10-05)
include `cold-sgb-v1` and `clocked-pixel-v1`; the new bridge preserves previous
production PCM prefixes with appended endpoints but fails all four measured
Linux headroom profiles. Earlier Balanced Windows/Android measurements do not
qualify it; see [performance status](../docs/sgb-host.md#current-evidence-status-audited-2026-10-05).

## SPC700 IPL

`spc700/ipl.asm` implements the original GBB 64-byte port-protocol loader.
`spc700/ipl_image.hpp` is bundled by the SGB firmware host. Desktop firmware
playback no longer requires `spc700.rom`; an existing file remains an explicit,
validated 64-byte override. The selected SGB1/SGB2 SNES program ROM remains
required. The source and generated image contain no reference dump or SGB
audio driver. Normal builds need no assembler or Python.

```sh
python3 scripts/build_spc700_ipl.py --check
python3 scripts/build_spc700_ipl.py --check --output NEW_IMAGE.bin
```

The strict Python assembler resolves the source's instruction subset and
records source/image hashes. See [IPL validation](../docs/spc700-ipl-validation.md)
for the execution-only reference checks, exact playback gates, reset and
snapshot identity contracts, and scope limits.

## SGB/SGB2 Game Boy-side bootstrap

`gameboy/sgb.asm` builds original 256-byte SGB and SGB2 images. Both clear VRAM,
initialize audio/LCD, construct six header packets in `C000–C05F`, send them
through ordinary JOYP writes, observe four VBlanks after each packet, and unmap
at `00FE`. Each packet contains its ID (`F1/F3/F5/F7/F9/FB`), a modulo-256 payload
sum and fourteen header bytes; the final payload is zero-padded. Pulses follow
the original boot's execution-measured timing: four-M-cycle low pulses,
data-dependent high spaces and a shorter stop-bit high space. These bootstrap
observations differ from the general packet protocol's conservative minimums;
they do not redefine timing requirements for ordinary game commands.
Handoff is `PC=0100, SP=FFFE, BC=0014, DE=0000, HL=C060, F=00`, with `A=01`
for SGB and `A=FF` for SGB2. No header validity check or proprietary logo is
embedded in this GB-side firmware. A real SNES-side program still performs its
own cartridge checks; bundling this bootstrap does not bypass those checks.

**GBB fast boot** selects these images for SGB profiles, including automatic
selection of SGB-capable monochrome titles. The animated preference uses the
same bootstrap: SGB's original introduction is SNES-side, not a DMG animation.
Instant startup remains the default. Experimental desktop firmware playback
uses the bundled image when no GB-side override is present; supplying
`sgb.boot.rom` / `sgb2.boot.rom` still explicitly overrides it. Invalid overrides
are rejected. Private SNES program ROMs are still required; the SPC700 IPL
is bundled with an optional external override.

```sh
python3 scripts/build_dmg_boot_rom.py --model sgb --check
python3 scripts/build_dmg_boot_rom.py --model sgb2 --check
```

Initialization writes, every packet edge and handoff divider/PPU/APU phases
are guarded by exact timing contracts across varied headers. Builds never read
reference firmware. `--check-source` works without RGBDS;
`--output NEW_FILE.bin` exports our original image for local execution tests.
See [SGB boot validation](../docs/sgb-boot-validation.md) for scope, repeatable
opaque comparisons, independent handoff checks and remaining scope limits.

## CGB replacement startup

`gameboy/cgb.asm` provides original 256-byte fast and animated images for
CGB-0 and later CGB profiles. GBB's deterministic cold bus selects native CGB
or GB compatibility mode from the cartridge before firmware execution; the
firmware establishes the stack, clears accessible VRAM banks, checks the
cartridge header checksum, initializes audio/palettes, and unmaps itself at
`00FE`, transferring execution to `0100` with `SP=FFFE`, `A=11`, and `F=80`.
Native color games receive `BC=0000, DE=FF56, HL=000D`. Monochrome games receive
`C=00, DE=0008`, with the documented licensee/title-dependent `B` and `HL`
values. CGB-0 preserves cold wave RAM; later profiles initialize it with
alternating `00/FF` bytes. GBB's existing automatic compatibility colorization
remains in use for monochrome games.

Select a **CGB** hardware profile and **GBB animated boot** for a roughly
3.2-second color-model intro: bold oblique **GO BIGGER BOY** lettering appears
letter by letter, cycles through saturated rainbow colors, settles to blue,
and fades to white, above a small original **GBB** footer. A two-note pulse
chime accompanies the reveal. The lettering is original GBB artwork. Like the monochrome
intro, graphics are host-rendered and audio is synthesized by a separate GBB
APU; they do not write animation assets into cartridge-visible VRAM or disturb
the emulated APU. Pause, mute, save/restore, and A/Start presentation hiding are
supported. **GBB fast boot** omits the presentation and its longer wait.

This is an original, GBB-assisted replacement, **not** a portable reproduction
of the original 2048-byte CGB firmware. Layout, color phases, fade and approximate
chime timing follow execution-only observations of a local reference; the longer
replacement wording has its own glyphs and reveal spacing. This is not a
pixel-identical or cycle-exact animation. It does not
implement the original KEY0/PGB boot transition, interactive boot palette
selection, legacy Nintendo logo tilemap, or exact original divider/APU phase.
No Nintendo code, logo, typeface, or recorded audio is included. Header checksum
failure leaves the LCD off and firmware mapped rather than launching the game.

Build and check each of the four variants with:

```sh
python3 scripts/build_dmg_boot_rom.py --model cgb --check
python3 scripts/build_dmg_boot_rom.py --model cgb --animated --check
python3 scripts/build_dmg_boot_rom.py --model cgb0 --check
python3 scripts/build_dmg_boot_rom.py --model cgb0 --animated --check
```

Use `--output NEW_FILE.bin` to export a generated image for local testing.
`--check-source` verifies source/image provenance without RGBDS. CPU-visible
contracts follow [Pan Docs](https://gbdev.io/pandocs/Power_Up_Sequence.html).
The `gameboy_cgb_boot` tests cover all four CGB profiles, both cartridge modes,
both startup modes, checksum rejection, fade/chime, mute/skip, and mid-boot
save/restore. Firmware source and RGBDS reproducibility checks cover all images.

For visual review (no ROM needed), render the original GBB artwork into a new
ignored build directory:

```sh
mkdir -p build-cgb-preview/frames
c++ -std=c++20 -Iinclude scripts/cgb_boot_visual_preview.cpp \
  build-desktop/libgameboy_core.a -o build-cgb-preview/preview
build-cgb-preview/preview build-cgb-preview/frames
```

The optional `scripts/cgb_boot_visual_reference.c` captures output from a local
SameBoy core through its public API, executing a user-provided cartridge and
boot image as opaque inputs. It also reports approximate pulse-trigger times.
Compile with the local SameBoy include root and `libsameboy.a`, linking `-lm -ldl`;
arguments are `CARTRIDGE BOOT_ROM NEW_OUTPUT_DIRECTORY FRAMES`. Keep reference
captures in an ignored build directory. Neither firmware contents nor reference
artwork are decoded, bundled, or used by the replacement renderer. Both capture
tools refuse to overwrite existing images.

## AGB / AGB-0 startup compatibility

Select **AGB** or **AGB-0 (startup compatibility)** to run GB/GBC cartridges
with the documented Game Boy Advance boot handoff. Both fast and animated
variants are bundled; animated startup uses the same original GBB Color intro.
Automatic selection is unchanged and never selects these profiles.

Native color games receive `A=$11`, `B=$01`, `F=$00`, `DE=$FF56`, `HL=$000D`.
For monochrome games, the licensed title checksum is incremented in `B`;
zero/half-carry flags follow that increment, including `$FF` wraparound.
Legacy `$43`/`$58` title checksums retain `HL=$991A` before the increment.
Instant startup uses the same cartridge-dependent registers. These conventions
follow [Pan Docs](https://gbdev.io/pandocs/Power_Up_Sequence.html).

These are **startup compatibility profiles**, not native GBA emulation or a
fully validated AGB hardware core. Runtime timing, PPU and APU currently use
the CGB-E baseline. Full AGB-specific audio/display behavior and exact original
boot memory/divider state remain unimplemented. The profiles are intentionally
not added to the silicon-accuracy conformance matrix yet.

AGB-0 and AGB have separate generated firmware names but identical replacement
bytes: the original later revision's logo-check hardening has no counterpart in
our deliberately logo-free boot. Both initialize wave RAM, unlike CGB-0.
Rebuild/check with `scripts/build_dmg_boot_rom.py --model agb` or `--model agb0`,
adding `--animated`, `--check`, or `--check-source` as needed.

The optional `scripts/agb_boot_handoff_reference.c` executes a user-provided
cartridge header and boot image in a local SameBoy core (`GB_MODEL_AGB`), then
reports 36 native/compatibility/license/checksum handoffs as CSV. Header variants
exist only in memory. Firmware instructions and logo assets are not decoded or
emitted. Compile against SameBoy's public headers and `libsameboy.a` with
`-lm -ldl`; arguments are `USER_CARTRIDGE USER_BOOT_ROM`.
`scripts/agb_boot_handoff_preview.cpp` emits the equivalent logo-free GBB matrix;
compile like the visual preview above and pass `agb|agb0` and
`instant|fast|animated`. Keep captures and user-owned inputs out of Git.

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

### Early Game Boy (DMG0)

The DMG0 variant is also built from this original source (`GBB_DMG0=1`).
It hands off `AF=0100`, `BC=FF13`, `DE=00C1`, `HL=8403`, `SP=FFFE`,
and `PC=0100` for either valid checksum path. Ordinary CPU writes and waits
establish DIV=`1828`, LY=`91`, STAT=`81`, and LCD dot 92. Failed checksums
leave the firmware mapped and blink a white/black screen instead of starting
the game or showing the GBB animation. Nintendo-logo authentication remains
intentionally absent, so logo-free homebrew with a valid checksum works.

Fast startup takes about 1.02 seconds. Animated DMG0 adds 314 whole DIV wraps
(20,578,304 clocks), taking about 5.93 seconds. It uses the original GBB lettering
with the early model's slower approximate scroll cadence and separately
APU-synthesized note triggers at clocks 18,523,904 and 18,875,268. The local
opaque reference handed off at clock 24,844,328; this variant is within one
video frame. These measurements establish specific observed behavior, not
complete equivalence to physical DMG0 hardware, its APU phase, or its boot ROM.
DMG and Pocket images remain byte-for-byte unchanged.

```sh
python3 scripts/build_dmg_boot_rom.py --model dmg0 --check
python3 scripts/build_dmg_boot_rom.py --model dmg0 --animated --check
ctest --test-dir build -R gameboy_dmg0 --output-on-failure
```

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
images for DMG0, DMG, MGB, CGB/AGB revisions, and the shared fast/animated
SGB/SGB2 bootstrap; they do not
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

For DMG/MGB variants, the generator assembles `dmg.asm`; CGB/AGB variants use
`cgb.asm`, and SGB variants use `sgb.asm`. It verifies the 256-byte size and final
unmap instruction, and records source and image SHA-256 hashes in the generated header.
It refuses to overwrite an existing `--output` image. CTest checks source/image
reproducibility when RGBDS and Python are available, and checks source/image
provenance whenever Python is available; the execution contracts
always run when core tests are enabled.

```sh
build/gbb_test_runner /path/to/homebrew.gb --model dmg --dmg-boot --protocol serial
ctest --test-dir build -R 'gameboy_dmg_' --output-on-failure
```

The firmware is also available through `BootRomMode::replacement` in the
core API. The selected hardware profile chooses its matching image on DMG0,
DMG, MGB (Pocket), CGB/AGB revisions, SGB and SGB2.
Desktop, Android and web settings expose **Startup** with
**Instant startup** (default), **GBB fast boot**, and **GBB animated boot**. Desktop and Android
persist `boot.Startup = instant`, `replacement`, or `animated` in `settings.ini`; web
persists the choice in browser local storage. No external firmware download is
required: the image is bundled in every build.

The setting IDs are hardware-neutral: `replacement` and `animated` select
matching firmware for the chosen model. Old DMG-specific IDs are not supported;
unrecognized saved values fall back to instant startup until reselected.

For DMG/MGB, the animated option selects a separate original 256-byte firmware variant. Its
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
with a dual-mode title, or **MGB** for Pocket. CGB profiles now select their own
replacement described above; SGB profiles select their bundled header bootstrap. Experimental
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
Broader title-level and independent-hardware validation remain future
milestones for the bundled model-specific boot implementations. SNES-side
SGB1/SGB2 production program ROM replacements are not implemented here yet.
The original [SNES-side diagnostic prototype](../docs/sgb-original-firmware.md)
now establishes reproducible startup, restricted two-voice playback and
next-frame SOU_TRN capture/upload/handoff;
it is not a bundled program-ROM default. The original
SPC700 IPL replacement is described in [IPL validation](../docs/spc700-ipl-validation.md).
