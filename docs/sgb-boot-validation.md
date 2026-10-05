# Original SGB/SGB2 boot validation

This milestone replaces only the 256-byte Game Boy-side boot image. It does
not replace the SNES program ROM or SPC700 IPL, introduce a branded SNES intro,
or establish complete physical-hardware equivalence. Source and generated
images are original GPL-3.0-or-later GBB code. Private inputs and captures remain
in ignored directories and are not release assets.

## Automated ROM-free contracts

`gameboy_sgb_replacement_boot` exercises both models and both replacement
startup preferences using synthetic headers, including invalid checksums and
zero/varied data. It checks all six packet IDs, payload sums, padding, pulse/space
widths, four-VBlank scheduling, CPU/JOYP/LCD handoff, deterministic reset, and
save/restore during a live packet. Diagnostics are deliberately reset on restore;
the serialized machine state and subsequent execution still match exactly.
The ROM-free timing oracle checks every boot I/O write's value and clock,
including header-dependent one/zero bit timing, VBlank polling and fixed idle
intervals. Handoff checks include the full divider counter, internal PPU dot and
mode, APU frame-sequencer/channel clocks and model-specific resampler phase.
Linear and seeded high-entropy headers cover both models and startup preferences.
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
It compares CPU registers, the 96-byte header buffer, actual JOYP packets,
the complete boot I/O timeline, all readable I/O and internal handoff phases.
Reports contain hashes and timing metadata, not firmware,
logo graphics, packet contents or memory captures.

```sh
python3 scripts/validate_sgb_boot.py \
  --probe build-desktop/gbb_dmg_boot_probe --reference-dir roms \
  --rom 'roms/Donkey Kong (JU) (V1.1) [S][!].gb' \
  --report build-sgb-boot-validation/contract.json
```

`roms` must contain the caller's `sgb.boot.rom` and `sgb2.boot.rom`; four
logo-free linear patterns and sixteen seeded random headers are always included.
The optional `scripts/sgb_boot_handoff_reference.c` can be compiled against a
separate local SameBoy core and run as `PROBE sgb|sgb2 GAME BOOT --trace`.
It uses SGB/SGB2's GB-only (`NO_SFC`) profiles, avoiding an unrelated HLE SNES
intro/header checker. It records I/O writes and compressed LY read intervals,
CPU/register handoff and independent divider/APU/channel/resampler phases.
Pass its executable as `--independent-probe PATH` to run both boot images in
that core too. Independent timelines are normalized to the initial JOYP idle
write; no PC/opcode trace or disassembly is used. Reports identify both probes
by SHA-256. Exact comparisons are between original and replacement execution
within each core, not comparisons of unlike cores' internal state layouts.
The external core is validation-only and never bundled or linked into GBB.

## Limits of the evidence

The previous final-VBlank discrepancy is corrected: SGB and SGB2 expose LY=0
at dot four of internal line 153 while retaining VBlank mode, with separate LYC
comparison edges at dots four/eight/twelve. Targeted tests cover these edges,
interrupts, save/restore and batched versus literal clocks. Early LY zero does
not publish a second frame or start rendering line zero early.

ICD scanline status and tile-row completion use the physical PPU scanline,
not CPU-visible LY. Thus the early LY=0 alias cannot expose visible row zero
to the SNES host while internal line 153 is still in VBlank. A ROM-free
boundary test covers both models. This remains the existing scanline-granular
ICD approximation, not a claim of cycle-exact ICD2 pixel transfer.

The replacement deliberately leaves VRAM clear instead of reproducing Nintendo
logo tiles. The report's `cycle_exact=true` now means the measured boot I/O
timeline and handoff phases match in GBB; it is not a claim about every internal
CPU/memory operation, initial artwork, analog output or complete physical
hardware equivalence. Independent execution is an additional implementation
oracle, not a replacement for real-hardware captures. No firmware content is
decoded, copied or bundled from the reference images.

Whole-host local Donkey Kong gameplay replays with private SGB1/SGB2 program and
SPC IPL images completed with the replacement boot: eleven input events, three
delivered sound commands, two audible commands, nonzero combined audio and over
5,000 GB frames on each model. These are integration checks, not an independent
audio-fidelity oracle. Original-override audio baselines remain separate tests.

A replacement-boot Pokémon Blue SGB2 new-game replay also completed over 1,500
GB frames and fourteen input events with nonzero combined audio. The existing
original-override title suite passed its exact audio hashes, including mid-stream
save/restore, for both SGB models. Those observations predate the exact-timing
refinement; current refinement results are recorded separately below.

## Exact-timing refinement validation

The refined bootstrap passed 46 original/replacement comparisons: twenty
synthetic headers and local Tetris, Donkey Kong and Pokémon Blue cartridges,
each on SGB and SGB2. Every case passed the complete boot I/O and internal
handoff-phase comparison in GBB and the independent SameBoy GB-only probe.
The ROM-free contracts additionally cover nineteen headers, both models and
both startup preferences, with reset and mid-packet restoration.

All 174 non-local native regression tests passed. The targeted Windows boot,
PPU, firmware-adapter and core contracts passed; Android debug assembly and
unit tests succeeded. All eight browser tests passed, and the three WebGL
voxel screenshot modes had zero mismatched pixels against existing baselines.
These checks do not constitute a fresh Android device performance measurement.

The corrected PPU phase changes fixed-instruction-budget gameplay endpoints.
Against archived original-override whole-host captures, all overlapping native
SNES PCM samples remain byte-identical: SGB1 ends six silent stereo frames
earlier and SGB2 ten earlier. Whole-WAV hashes necessarily change with their
lengths. Combined GB+SNES PCM changes because Game Boy timing is corrected;
it is not advertised as unchanged audio. Current combined captures and final
GB states match byte-for-byte with APU batching, SPC idle caching, direct DSP
clocking and DSP phase dispatch all disabled. Exact hashes remain required;
the tests do not tolerate arbitrary sample differences or discard silence.

The refined replacement and original override also produce byte-identical
combined Donkey Kong gameplay WAVs on both models at their respective 48 kHz
and 44.1 kHz presentation rates. All eleven inputs, three delivered SOUND
commands, two audible commands and over 5,000 GB frames complete. The four
native diagnostic timing profiles retain their existing input, upload,
SOUND, boot-cycle and (where enabled) independent startup-landmark guards;
only their fixed-instruction-endpoint WAV pins are refreshed.

Both original-override native whole-host restore replays and both reusable-APU
title parity checks pass their refreshed exact WAV pins. The latter compare
the native trace scheduler with the reusable core engine, including its
state-roundtrip checks. The older fractional-APU title diagnostic also passes
its packet, timer, delivery and clock-spacing guards.

The complete combined-audio suite passes all four modes (native, combined,
restored and scalar) on both models. Its 586 SGB1 and 538 SGB2 combined-audio
restores retain byte-identical PCM and final GB state; changing the consumer
chunk size to 257 also preserves output. The final run used workspace scratch
storage for the large private WAVs. Set `TMPDIR` to a sufficiently spacious
scratch directory when running these local suites on a small `/tmp` filesystem.

This work does not replace the SNES program ROM or SPC IPL. The experimental
whole-host adapter still uses its existing external-boot bus reset baseline;
matching the standalone cold-boot contract does not establish equivalence of
every host reset state or analog audio characteristic.
