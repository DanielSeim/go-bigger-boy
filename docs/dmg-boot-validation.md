# DMG replacement boot validation

The replacement remains **experimental and opt-in**. Normal frontend startup
still uses the production post-boot path. This tool compares two CPU-executed
cold starts on the same GBB core; it is not an independent emulator or a claim
of complete hardware equivalence.

## Running the local comparison

Build the test-only probe with core tests enabled:

```sh
cmake -S . -B build-boot -DCMAKE_BUILD_TYPE=Release -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-boot --target gameboy_dmg_boot_probe
python3 scripts/compare_dmg_boot.py \
  --probe build-boot/gbb_dmg_boot_probe \
  --reference-boot /path/to/your/dmg_boot.bin \
  --rom /path/to/your/title.gb \
  --output /tmp/dmg-boot-comparison.json
```

Repeat `--rom` for more cartridges. The default boot budget is 40 million
T-cycles, enough for the original DMG startup, and `--run-cycles` executes two
million additional cycles from each handoff (override it, or use zero for
handoff only). Both inputs start with zero-filled RAM and the same cold CPU,
LCD, timer, serial and APU baseline. Adjacent saves/RTC files are neither read
nor written. Cartridge input hashes are checked for changes during the run.
Add `--align-frame` to advance each followup to the next completed LCD frame;
the exact handoff snapshot is unaffected. The alignment wait is bounded to two
frames and fails explicitly if the LCD does not publish a frame. This avoids
comparing a partly rendered framebuffer with a completed one just because the
two boot paths have different LCD dot phases.

The probe stops at the instruction that unmaps the boot ROM and requires
PC=`0100`, **before executing the cartridge**. It observes CPU registers, all
128 I/O bytes, IE, unrestricted VRAM/OAM, WRAM/HRAM, the full divider counter,
LCD dot/mode, serial clock phase and selected internal APU clocks. It also
records stereo PCM sample counts, nonzero counts, peak, RMS and a
little-endian int16 PCM hash separately for boot and followup. These are
observations, not an audio-equivalence gate: games can start sound at different
sample phases even when their visuals match. No PCM or firmware bytes enter
the comparison report. Its JSON stdout includes RAM snapshots for the local
comparator only; do not publish
raw probe output, which can contain original logo tiles. The final comparison
report contains memory hashes and mismatch address ranges, not ROM or RAM
contents. Existing report files are never overwritten.

Exit codes: `0` stable contract matched; `1` stable contract mismatch; `2`
invalid input, timeout, failed probe or report error. Even a successful report
explicitly records non-equivalence in `exact_snapshot_equivalence`.

## What is a stable contract?

The gate compares CPU registers and interrupt state, hardware-defined stable
I/O (including LY/STAT), IE, all WRAM/OAM and HRAM outside `FFFA`–`FFFD` boot
stack scratch. It does not waive CPU or defined I/O mismatches. All observed
I/O differences remain in the report, including non-gated ones.

DIV and hidden timer/serial/APU phase are measured, not declared equivalent.
Wave RAM and object palette power-on values are not hardware-defined. Boot
VRAM and stack scratch deliberately differ: this replacement does not install
Nintendo logo/trademark data or reproduce the original animation/chime.
Followup CPU state, framebuffer hashes and memory hashes are observational: different boot graphics
and phase can affect title execution, so followup divergence is not suppressed
or interpreted automatically as a cartridge failure.

## Revision 1 findings (2026-10-04)

Using the locally supplied original DMG image, SHA-256
`cf053eccb4ccafff9e67339d4e78e98dce7d1ed59be819d2a1ba2232c6fce1c7`,
the stable contract matched for Pokémon Blue (UE), Super Mario Land v1.1,
Tetris v1.0 and Donkey Kong (JU) v1.1. Each cartridge also executed at least
two million followup cycles (rounded to the next instruction boundary), and
the resulting framebuffer hashes matched between the two boot paths for all
four titles. This is a short startup check, not full gameplay coverage.
An extended check executes at least 60 million followup cycles (about 14.3
emulated seconds) and then captures the next completed LCD frame. The resulting
framebuffer hashes match for all four titles too. Unaligned Super Mario Land
snapshots initially differed because one was in mode 3 at dot 172 and the other
in HBlank at dot 396 on the same line; CPU and all measured RAM already matched.
The completed-frame option resolves that measurement artifact without changing
the firmware or hiding the differing clock phases.
Neither the original firmware nor these cartridges are build inputs or shipped
test assets. The replacement image itself is unchanged by this validation work.

Two core defects emerged from the comparison:

- Cold JOYP incorrectly inherited the generic no-selection default; DMG now
  starts with both input groups selected (`CF` with no buttons held).
- The CPU-visible LY never wrapped early on internal line 153. It now reads
  zero from dot 4 while the internal scanline remains in VBlank. The separate
  LYC=153 / no-comparison / LYC=0 phases are retained, as are comparisons on
  intermediate VBlank lines. Consequently both boot paths now hand off with
  LY=0, STAT=`85`; the old replacement assertion of STAT=`87` was a core bug,
  not a desirable firmware difference.

The timing fix covers the standalone DMG/MGB path only. CGB revision/speed-specific
behavior is not silently assigned DMG timing. SGB's existing host/GB timing
and byte-exact title audio remain unchanged; migrating that coupled path needs
its own independent reference validation. Regression tests cover dot boundaries, STAT
interrupts, bulk versus single-dot clocks, frame publication and state restore.
Hardware evidence is from [TCAGBD section 8.9.1](https://github.com/AntonioND/giibiiadvance/blob/master/docs/TCAGBD.pdf)
and the [Gambatte hardware tests](https://github.com/pokemon-speedrunning/gambatte-core/tree/master/test/hwtests/ly0).
The initial twelve-fixture check passed eleven cases. The previously failing
`lycint152_lyc153irq_late_retrigger_2` acknowledgment race is now fixed; the
expanded check and its remaining limitations are recorded below.

## STAT acknowledgment regression (2026-10-04)

The standalone DMG/MGB CPU now retains a STAT acknowledgment phase across
vector entry and the first vector opcode fetch. An overlapping STAT edge is
consumed; an edge after that fetch can retrigger. Interrupt dispatch still
takes twenty clocks, with unchanged arbitration, stack writes and vector
selection. Other IF bits are preserved. Software IF writes do not arm this
phase. PPU LY/coincidence transitions and ordinary IF read/write timings are
unchanged: moving the PPU interrupt earlier fixed the target but regressed
neighboring tests, so that candidate was discarded.

This is a hardware-fixture-backed **behavioral timing model**, not a claim that
the internal CPU acknowledgment signal has been independently measured at
gate level. It is restricted to the standalone DMG-style path; CGB and the
coupled SGB/SGB2 host timing are not migrated by this change.

The expanded local check covers all 33 DMG-applicable `lycint152_*` fixtures
in the public Gambatte `ly0` group. Results improve from **29/33 to 31/33**,
both with direct post-boot startup and with the replacement DMG firmware:

- `lyc153irq_late_retrigger_2` and `lyc0irq_late_retrigger_2` now return the
  expected `E0`; the neighboring `late_retrigger_1` cases still return `E2`.
- Ordinary interrupt reads, IF-write races and LY/LYC comparisons retain
  their previously passing results.
- `lycint152_m0irq_1` and `lycint152_m2irq_1` remain mismatches (`E2` versus
  `E0`). These are existing mode-source timing limitations, not passing tests.
- All 63 CGB-C results are unchanged from the baseline, which passes only
  19/63 in this broader group. This does **not** establish CGB conformance.

Reproduce using a local copy of the external fixtures (not shipped in GBB):

```sh
cmake --build build-boot --target gameboy_stat_irq_probe
python3 scripts/validate_stat_irq.py \
  --probe build-boot/gbb_stat_irq_probe \
  --rom-directory /path/to/gambatte/ly0 \
  --output /tmp/stat-ly0.json
# Add --dmg-boot for the replacement, or --model cgb-c for the CGB group.
```

The source-specific probe observes A at the public tests' result routine;
it is not a generic cartridge completion detector. Reports contain computed
bytes, fixture names and SHA-256 provenance, not ROM/RAM data. Inputs and
tools are fingerprinted before and after execution, reports refuse overwrite,
and mismatches return exit code 1 rather than being hidden as successes.

Offline synthetic regressions cover both LYC sources, individual-clock
acknowledgment boundaries, batched/literal PPU execution, reset, model isolation,
priority and save/load across vector entry. Save-state version 41 appends one
boolean acknowledgment phase without moving legacy CPU/bus fields. Versions
1-40 still load, defaulting to no pending phase; malformed phases are rejected
with atomic rollback. The replacement firmware image itself is unchanged.

The eleven focused boot/CPU/timer/PPU/save checks pass. The five selected
ASan/UBSan checks also pass (LeakSanitizer disabled for the sandbox). Ten of
eleven additional Mooneye interrupt/HALT fixtures pass; the sprite variant of
`intr_2_mode0_timing` times out with identical diagnostics on the committed
baseline and the updated core, so it remains an existing limitation.

A direct before/after check against the committed core (`75ca1a3`) runs
Pokémon Blue, Donkey Kong v1.1, Tetris v1.1 and Super Mario Land v1.0 for thirty
seconds after replacement boot, pressing Start at four seconds. All four have
byte-identical stereo PCM, identical **final** framebuffer hashes and equal
instruction budgets. This checks the sampled final images, not every video
frame. An intermediate candidate accidentally changed timer clocking when
STAT and timer requests were both pending; the exact PCM comparison caught
that, and a dedicated mixed-source regression now protects the existing
timer path. Private captures and firmware remain local, never shipped assets.
The final optimized 165-entry CTest run passes 162 entries with three
network-dependent skips and no failures, including the local SGB combined-title
and audio regressions. Skipped network tests are not reported as passing.

## Revision 2: timer and quiet-audio handoff (2026-10-04)

The firmware now establishes a hardware-test-backed **fast-start** contract:
raw divider `ABC8`, visible LY=0 / STAT=`85`, internal LCD line 153 dot 396,
and channel 1 envelope volume zero. Both checksum branches have equal timing.
DIV is reset by a CPU instruction; counted firmware delays and LCD polling
establish the phase. There is no hidden timer seed or post-boot state injection.
The readable sound registers remain canonical (`NR12=F3`, `NR52=F1`). Normal
64 Hz envelope clocks decay the channel with `NR51=0`; reconnecting the mixer
and waiting for its high-pass filter settles the DC transient before handoff.
This avoids dependence on partly model-specific envelope-write quirks.
Startup takes about 1.03 emulated seconds, versus about 5.59 for the local
original. It does not recreate the original animation or chime.

Mooneye `boot_regs-dmgABC`, `boot_div-dmgABCmgb` and
`boot_hwio-dmgABCmgb` now pass through the replacement. The divider and boot-I/O
tests exercise more than just DIV's visible high byte: they check increment
phase and later LCD/I/O reads. Offline synthetic tests independently cover an
exact cartridge DIV-read boundary, both header flag paths, a full second of
quiet output (peak <= 8 int16 counts, below -72 dBFS), normal subsequent channel
retrigger, and byte-exact state/PCM resume during envelope settling.
The small quiet-output residual is the current float high-pass filter's DC
rounding floor, not an active pulse tone; this is not advertised as zero PCM.
The DAC/mixer power-on transient occurs during boot and is measured separately.
Changing the shared audio filter to conceal it would risk unrelated SGB output.

Validation also passed Blargg `cpu_instrs` (all eleven subtests), `instr_timing`
and `mem_timing` through the replacement. The optimized full CTest run passed
158 entries, with three network-dependent entries skipped in the sandbox;
SGB combined-title and audio PCM baselines passed. All six focused boot/PPU
checks passed again on the final generated image, and the three selected
boot/probe/PPU sanitizer checks passed with ASan/UBSan (LeakSanitizer disabled
because the sandbox cannot support its ptrace checks).

Fresh revision-2 comparisons still pass the stable CPU/I/O/RAM contract for all
four titles above. After 60 million followup cycles and completed-frame alignment,
their framebuffer hashes all match. Envelope volume is zero at handoff in both
paths. Serial, APU sequencer, pulse waveform and resampler phases still differ;
equal register values or framebuffer hashes do not establish audio equivalence.

### Cold original's four-clock discrepancy

The original still hands off at raw DIV `ABC4` on the unchanged deterministic
cold baseline; the production post-boot path and hardware-test-backed replacement
use `ABC8`. The test-only probe accepts `--cold-clock-cycles N` (0..16), recording
that offset explicitly and advancing the cold peripheral clock before any boot
instruction. It is a sensitivity experiment, not a production reset option.
With `N=4`, opaque original execution reaches `ABC8` and Mooneye's divider
test produces its passing result registers. CPU boot instruction totals remain
23,440,324 and the handoff LCD phase remains dot 396. This isolates the issue
to reset/clock origin sensitivity; it does **not** establish whether physical
hardware has an initial CPU fetch delay, a timer reset offset, or another reset
sequence difference. The core baseline has not been changed to fit this result.
An independently verified reset trace is still needed before declaring the
original cold path hardware-correct.

The envelope policy follows the public
[Pan Docs DIV-APU and mixer description](https://gbdev.io/pandocs/Audio_details.html).
The phase gates come from the hardware-verified
[Mooneye boot-divider](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/boot_div-dmgABCmgb.s)
and [boot-I/O tests](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/boot_hwio-dmgABCmgb.s),
not Nintendo instruction listings.

## Remaining differences and next gates

Revision 3 subsequently aligns the inherited APU sequencer and CH1 waveform
state and validates actual cartridge output; see
[cartridge audio validation](dmg-boot-audio-validation.md). It preserves the
timer/LCD and quiet-handoff gates above; the resampler phase and the original
cold-reset discrepancy remain distinct limitations.

Revision 4 aligns the standalone serial handoff/first-transfer phase with the
unchanged cold reference and fixes stale external shift-register saves; see
[serial validation](dmg-boot-serial-validation.md). Shared serial timing, the
production post-boot phase and attached cable policy are unchanged.

The replacement remains opt-in. Cold reset provenance, broader title-level
sound comparisons, and model-specific MGB/CGB implementations remain separate
gates. The outstanding STAT interrupt-clear/retrigger race above is unchanged.
Do not copy original code/assets or force private snapshots to hide these
differences. Logo-free homebrew must continue to boot, and no external firmware
becomes a release dependency. SGB's coupled host timing and audio are unchanged.

The probe and comparator's own tests require only original synthetic fixtures:

```sh
ctest --test-dir build-boot -R 'gameboy_dmg_|gameboy_ppu_timing_contract' --output-on-failure
```
