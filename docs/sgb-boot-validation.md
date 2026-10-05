# Original SGB/SGB2 boot validation

This milestone replaces only the 256-byte Game Boy-side boot image. It does
not replace the SNES program ROM or SPC700 IPL, introduce a branded SNES intro,
or establish complete physical-hardware equivalence. Source and generated
images are original GPL-3.0-or-later GBB code. Private inputs and captures remain
in ignored directories and are not release assets.

## Current test pins and provenance (audited 2026-10-05)

The committed host uses `cold-sgb-v1` plus `clocked-pixel-v1` and saves
`GBBSHOST` version 3; host versions 1/2 are rejected. Current private Donkey
Kong v1.1 replays use 60 million host instructions, eleven inputs, three SOUND
deliveries and two audible commands. These are implementation regression pins,
not independently established hardware audio. The complete WAV hashes are:

| Model | Native SNES, 32 kHz | Combined GB+SNES |
| --- | --- | --- |
| SGB1 | `1b4cc438b6aadcf0916912698fede5396386c6f2656b3c5853c71ca971260311` | 48 kHz: `9a9b1d035d4867a73927725498fb082ae7f9b57d7f9a6b6e05d66fe384cd8a59` |
| SGB2 | `d9211313e26aa4c200d40df114b711a564ecd7418063747579bedbb0c21d096e` | 44.1 kHz: `f7b183e755d3f46f6d6438beda320798156bdde319a17aeb0aeac223ccfdc341` |

Sources: [native title tests](../tests/sgb_host_local_title_tests.py),
[combined/restore/scalar tests](../tests/sgb_host_combined_title_tests.py), and
[serial benchmark](../scripts/benchmark_sgb_host.py). The benchmark additionally
pins final GB-state hashes: SGB1 native `17273510614454273623`, combined
`2188253438664865528`; SGB2 native `17670032470297762322`, combined
`9371530888034411371`. The benchmark pins the initial battery save too.
These suites explicitly supply private GB-side boot overrides; desktop playback
instead uses the bundled bootstrap when no override exists. Desktop always
mixes at 48 kHz, so its SGB2 output is not the 44.1 kHz diagnostic pin.
Combined pins are Linux/GCC-specific; other compiler/architecture captures
require the documented same-platform comparison.

The latest bridge capture retains every prior production PCM sample and appends
only endpoint frames; see [playback evidence](#playback-baseline-evidence).
All four exact current WAV/state pins pass there, but all four Linux headroom
profiles fail (p05 1.153–1.228x, worst 0.931–1.071x). Earlier Balanced Windows
and awake Android measurements do not qualify the new bridge. No private,
expensive or device tests were rerun for this documentation audit.
See [runtime firmware requirements](sgb-host.md#experimental-desktop-playback)
and [generic startup IDs](../firmware/README.md).

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
boundary test covers both models. The subsequent clocked LCD bridge below
replaces whole-scanline row publication; this is still not a claim of complete
cycle-exact ICD2 hardware behavior.

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

## Historical exact-timing refinement validation

This capture milestone predates complete cold reset and the clocked LCD bridge.
Its test counts, endpoint observations and passing results are archived evidence;
current test pins are listed above. Earlier integration observations in the
limits section likewise retain their stated pre-refinement scope.

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

This work does not replace the SNES program ROM or SPC IPL. The boot refinement
above originally retained the adapter's limited LCD/DIV/interrupt reset; that
limitation is addressed by the subsequent complete cold-reset work below.
Neither milestone establishes analog audio or complete physical-hardware
equivalence.

## Whole-host cold-reset validation

The reset contract here remains current. Capture hashes and test counts in this
section record the pre-clocked-bridge milestone; later endpoint changes and
current performance evidence are recorded under Clocked LCD bridge below.

`SgbIcdGbSource` now uses the same complete SGB/SGB2 cold-power-on path as the
standalone replacement boot, then installs the caller's GB-side image. Both
native and legacy input policies use it. Policy selection no longer performs
register writes or resets a running GB. Every asserted ICD reset reconstructs
CPU and all GB peripherals and volatile RAM, preserves battery RAM and the
audio-enable preference, rebinds the raw-APU sink, and clears the sample epoch
before the next release. Presentation reset notifications retain the actual
host-bus timestamp. Whole-host reset retains its separately documented
configured-initial-save behavior.

The ROM-free host contracts compare complete serialized GB state with a
standalone cold oracle at construction and after dirtying CPU, PPU, APU,
WRAM/HRAM, wave RAM, serial, timer, interrupts and DMA. Persistent RAM survives,
supplied boot images remain mapped, callbacks keep their destination owner,
muted resets stay muted, and new-release sample timestamps remain monotonic.
Held live buttons survive reset and remain releasable afterward.
A separate raw-audio oracle compares exact PCM and final GB state against
standalone cold execution on both models, without relying on the previous
warm-audio hashes.

Fresh local Donkey Kong native SNES WAVs, host clocks, input/SOUND counts and
final GB states match the preceding baseline exactly on both models. Combined
GB+SNES WAVs change only during startup: differing interleaved samples span
approximately 1.93–7.25 seconds in SGB1 and 1.73–5.95 seconds in SGB2;
all later samples and total lengths match. Combined final GB states also match.
These corrections remove the inherited warm APU state, not change the audio
renderer or lower quality. Exact updated combined pins are checked separately
against restoration and scalar playback; historical captures above retain
their original meaning.

New host-runner reports and native diagnostic boot timelines identify
`gb_reset_profile: cold-sgb-v1`. The comparison tool preserves this provenance
and labels older captures without it `unspecified-legacy`; the old
`external_boot_reset: true` flag alone did not certify a complete reset.
Firmware, cartridge, waveform and saved-state inputs remain private and
untracked. Independent hardware reset/RAM and analog-output evidence remains
outside this software-baseline claim.

The older synthetic-input, 40-million-instruction SOUND diagnostics also use
the cold reset now. Their first audible packet is still captured at GB frame
2472, but host deliveries occur at frames 2472 and 2522 instead of the inherited
warm-start 2473 and 2521. Cycle-bus/shared/fractional KON observations are now
39/41; exact integer rendezvous observes 39/42. Fractional positive timer polls
retain the same 558/2876 half-clock spacing and 2/1 tick counts, with driver
phases 124→212→0. The updated fractional WAV SHA-256 is
`605fd6bdd74d2a76a24dd275552e6d988658d38a899a991a403fdb4057f199dd`.
These are explicitly synthetic-input diagnostic pins, not the unchanged
native-input playback/reference baselines and not evidence of hardware parity.
No fitted timing offsets or relaxed PCM tolerances were introduced.

The cold-reset validation run passes all 174 nonlocal/non-performance CTests,
the performance-report gate, and the local native host, reusable APU-engine and
combined host suites. Combined native/restored/scalar output agrees exactly on
both models, including 586 SGB1 and 538 SGB2 combined restores and the 257-sample
consumer partition. All four native diagnostic replay profiles and host
startup pass without changing their existing native PCM/timeline pins.
All six synthetic-input SOUND diagnostic profiles pass their cold-reset
expectations, including exact fractional PCM and cross-processor clock spacing.
Windows host, replacement-boot, firmware-core and core-contract tests pass;
Android debug build/unit tests and all eight browser tests pass. The three
WebGL voxel visual comparisons report zero mismatched pixels. These runs verify
software reset/playback invariants, not a new device-performance or physical
audio assessment.

## Clocked LCD bridge

The PPU sends raw two-bit pixels at their actual FIFO emission clocks, before
SNES masking, palette mapping or presentation. The ICD packs those bits directly
into its four 320-byte planar banks. A row first becomes initialized/complete at
pixel 159 of its eighth physical line, rather than at the following scanline.
`$6000` status follows timestamped physical line boundaries, never the CPU's
early LY=0 alias or an instruction's future PPU state.

GB execution is still instruction-granular. A fixed 32-event queue holds only
LCD output beyond the requested host/GB rendezvous; cached rendezvous calls
drain due events even when no new GB instruction executes. STOP gaps are
included in the GB clock epoch. There is no per-pixel allocation. Pending
events and partial planar banks are saved in experimental host state version 3;
callback bindings and derived clock caches are rebuilt on the destination.

Initialized and complete are distinct. Completed RAM survives LCD off. Reusing
a bank marks its current generation incomplete but retains known earlier bits,
overwriting only bits that have physically arrived. Real SGB firmware reads
such initialized in-progress banks after LCD restart; rejecting those reads
breaks otherwise working firmware playback. Never-initialized bank reads still
fail closed. This deterministic read/write-collision model is **not independently
validated hardware behavior**: the [ICD2 register documentation](https://gbdev.gg8.se/wiki/articles/ICD2)
explicitly describes reading the producer's incomplete buffer as unpredictable.
No analog LCD timing or undocumented ICD propagation delay is invented here.

`SgbIcdGbSource::set_lcd_observer` provides opt-in timing observations without
changing transfer behavior. Events contain GB clock, x, physical y and raw
pixel; x=160 denotes a physical line boundary and x=161 an LCD enable/disable
edge (pixel=1/0). `lcd_diagnostics()` distinguishes initialized and completed
banks and reports the stream offset and pending event count. The private title
runner includes that context in transfer faults. Captures can contain cartridge
graphics and must remain private.

ROM-free contracts cover one-clock-before/at-completion reads, status edges,
changed tile bits on bank reuse, planar packing, register aliases, the 192-byte
FF tail and 512-byte stream wrap. A literal-dot oracle checks batched timing
through fine-scroll, window and OBJ stalls and all four SNES mask modes; LCD
off emits no fake pixels. Whole-host continuation tests retain pending output
and destination-owned callbacks across same/cross-instance restores. The four
deferred title-level visual mismatches remain outside this milestone.

### Playback baseline evidence

Private Donkey Kong captures retain every interleaved PCM sample from the
preceding cold-reset native/combined baselines, bit for bit. Clocked `$6000`
polling changes where the fixed 60-million-instruction budget ends: SGB1 ends
11,392 master clocks later (about 0.530 ms), SGB2 3,470 clocks later (about
0.162 ms). Only the following stereo frames are appended; WAV hashes also
change because their length headers change.

| Model | Native SNES frames appended | Combined frames appended |
| --- | ---: | ---: |
| SGB1 | 17 | 25 at 48 kHz |
| SGB2 | 6 | 7 at 44.1 kHz |

GB frame counts remain 5,419/5,360, with 11 scripted inputs and three SOUND
packets on each model. Final GB-state pins change with the new endpoint, not
with a rendering or audio-quality change. New whole-host reports identify
`gb_lcd_profile: clocked-pixel-v1`; the exact WAV/state pins in the title tests
and serial benchmark refer to that profile together with `cold-sgb-v1`.

The older synthetic-input SOUND diagnostic models are a separate claim.
Their selected rendezvous timing changes KON observations to 37/39 in
cycle-bus/shared/fractional modes and 41/44 in exact integer APU mode. First
audible packet capture/delivery frames remain 2472 and 2472/2522; fractional
positive timer polls retain 558/2876 spacing, 2/1 ticks and phases 124→212→0.
The fractional diagnostic WAV SHA-256 is now
`823ceeb4a785cf531ccc63ce9d106a834a794350dd0e96cb7634e6683d0b9133`.
Unlike production native/combined playback, those diagnostic PCM payloads
can differ after the changed port rendezvous. Their independent upload
digests and startup/music timing bounds remain unchanged. No timing offset,
PCM tolerance relaxation, sample reduction or presentation downgrade is used.

Clocked-bridge validation passes all 174 standard nonlocal/non-performance
CTest regressions, the performance-report gate, all three private playback
suites and all eleven selected local diagnostic profiles. Native, restored
and scalar playback agree exactly on both models, including 586 SGB1 and
538 SGB2 combined restores and 257-sample consumer partitions. Expanded
ROM-free LCD tests cover both models at all four ICD dividers and STOP/resume.
Native and Windows host tests pass, as do Windows replacement-boot tests,
Android debug build/unit tests, the WebAssembly build and all eight browser
tests. Three WebGL voxel visual comparisons have zero mismatched pixels.
These checks do not resolve the four deferred visual mismatches or certify
physical ICD read/write collisions.

The fresh serial Release/IPO Linux host-only measurement uses one run per
profile, 90 post-warmup windows each, no competing build/test jobs, and an
unspecified power profile. All four complete WAV and final GB-state pins pass;
all four fail the unchanged 1.40x p05 / 1.20x worst-window headroom gate:

| Profile | Median realtime | p05 realtime | Worst window |
| --- | ---: | ---: | ---: |
| SGB1 native, 32 kHz | 1.314x | 1.228x | 0.986x |
| SGB1 combined, 48 kHz | 1.244x | 1.153x | 1.025x |
| SGB2 native, 32 kHz | 1.321x | 1.192x | 0.931x |
| SGB2 combined, 44.1 kHz | 1.231x | 1.159x | 1.071x |

Thus this milestone establishes clocked transfer correctness, **not restored
performance qualification**. Historical Balanced Windows/tablet measurements
do not qualify the new bridge. There is no same-host pre-change A/B capture
in this run to isolate bridge cost from host-capacity variation. Profiling
and an equivalent-output A/B measurement are needed before attributing or
fixing that cost. No gate, power setting, affinity, oscillator or quality
setting was changed to conceal these results. These ratios exclude frontend
rendering and physical audio-device playback; they are not device FPS claims.
