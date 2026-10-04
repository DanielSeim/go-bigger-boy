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
LCD dot/mode, serial clock phase and selected internal APU clocks. Its JSON
stdout includes RAM snapshots for the local comparator only; do not publish
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

## Local findings (2026-10-04)

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
Eleven of twelve DMG-applicable line-153 LY/LYC fixtures now produce their
expected visible result. `lycint152_lyc153irq_late_retrigger_2` still produces
`E2` instead of `E0`; the interrupt-clear/retrigger race needs separate work.
This is reported rather than silently treated as passing.

## Remaining differences and next gates

DIV's value and low-byte phase differ between fast startup and the original;
serial's free-running phase and APU sequencer, envelope, waveform and resampler
phase differ too. The original's completed chime leaves a different envelope
state from the fast-start active channel. Identical readable sound registers
are therefore insufficient to establish identical subsequent audio.

Mooneye `boot_regs-dmgABC` passes through both paths. `boot_hwio-dmgABCmgb`
passes through the original but fails through fast startup because the expected
DIV value is animation-duration-dependent. `boot_div-dmgABCmgb` fails through
both paths on this deterministic cold baseline: even executing the original
does not yet reproduce the hardware-observed divider phase. That is a remaining
core power-on/timing question as well as a fast-firmware policy question, not
evidence that copying the original firmware would solve startup accuracy.

Do not fix these by copying original code/assets, forcing post-boot register
snapshots, or tuning a hidden divider seed just to match one ROM. The next
milestone is a hardware-test-backed timer/audio handoff policy for fast startup,
with explicit treatment of the omitted animation/chime. Logo-free homebrew
must continue to boot, and no external firmware becomes a release dependency.

The probe and comparator's own tests require only original synthetic fixtures:

```sh
ctest --test-dir build-boot -R 'gameboy_dmg_|gameboy_ppu_timing_contract' --output-on-failure
```
