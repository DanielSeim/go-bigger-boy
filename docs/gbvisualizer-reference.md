# GBVisualizer reference candidate

[LIJI32/GBVisualizer](https://github.com/LIJI32/GBVisualizer) is an external CGB
integration/reference candidate. A pinned external build and a dedicated SameBoy
CGB capture adapter are available for opt-in research. It is **not a qualified
pixel regression gate**. Upstream source, music, ROMs and screenshots remain
external; the offline tool contracts run without them.

## Useful coverage

The [upstream README](https://github.com/LIJI32/GBVisualizer/blob/master/README.md)
describes a hardware-tested CGB music oscilloscope using PCM12 (`FF76`) and PCM34
(`FF77`). The [program](https://github.com/LIJI32/GBVisualizer/blob/master/visualizer.asm)
samples those registers in timer interrupts, changes SCX in STAT interrupts and
updates music on VBlank. It switches to double speed through KEY1/STOP.
This connects several independently implemented GBB features:

- Live digital APU channel amplitude reads, including wave/noise activity.
- CGB speed switching and CPU/APU/PPU clock relationships.
- Timer, STAT and VBlank interrupt scheduling.
- Scanline scrolling and CGB palette rendering driven by sampled sound.

GBB already has synthetic PCM12/PCM34 and hardware-revision contracts in
`tests/apu_hardware_contract_tests.cpp`, plus pinned SameBoy audio references in
`tests/fixtures/audio-external`. Those remain the focused correctness gates.
GBVisualizer would add an end-to-end integration signal; a moving oscilloscope
alone cannot establish exact sample timing, audio equivalence or hardware accuracy.
This is CGB coverage, not SGB replacement-program or SPC700 validation.

## External input and provenance

The repository listing inspected for this assessment contains no explicit project
license. The README says the included music is adapted from a Super Bomberman
disassembly. Do not treat a public GitHub repository as redistribution permission.
Keep this candidate external; establish applicable permissions before considering
vendoring source, music, ROMs or derived reference assets. No automatic download
or new public-CI dependency is introduced here.

The metadata pin is `tests/fixtures/external-reference/gbvisualizer.json`:
upstream commit `1649f3e7a9516701cf448c4529f8f119363fc713`, RGBDS `v1.0.1`,
32,768-byte ROM SHA-256
`fa59a6a8f1ec0c1ab8dac8d99c9d4cf4c8260008fd2b2809fa0506ef9866e180`.
The builder requires a clean tracked checkout and all three exact tool versions,
uses temporary copies, and rejects unexpected ROM hashes. Supply your own checkout:

```sh
python3 scripts/build_gbvisualizer_reference.py \
  --source /external/GBVisualizer --output /external/gbvisualizer-built
```

The output directory must be new. It contains the ROM and a manifest recording
input hashes, commands, adaptations and versions. The builder adds modern `DEF`
syntax, makes indirect `LDH` explicit, and expands HRAM offsets to absolute
addresses. `rgbfix -p 0` pads the linker output to the header-declared 32 KiB.
It uses the supplied `music.gbm` without running the legacy music converter.
Two clean builds produced identical ROMs. This establishes reproducibility with
the pinned modern toolchain, not byte equivalence with an unidentified older build.

## Initial GBB captures

For a locally supplied and pinned `visualizer.gbc`, the existing headless runner
can produce bounded screenshots without SDL:

```sh
./build/gbb_test_runner /path/to/visualizer.gbc --model cgb-e --dmg-boot \
  --frames 600 --max-cycles 120000000 --frame-output gbvisualizer-600.ppm
```

Confirm normal completion, nonblank output and animated changes across several
checkpoints (for example frames 60, 300 and 600). Retain failures as research
findings until their model, startup and timing assumptions have been reviewed.
Use separate output filenames for each checkpoint; do not bless current GBB
output automatically as a correctness reference.

`--cycle-frame-capture 600` is also useful for diagnosing LCD-off startup, but
counts **CPU T-cycles**. At CGB double speed it is not the same elapsed PPU time
as 600 completed display frames. Never compare it directly with a SameBoy capture
at frame 600. The existing cycle-capture diagnostics can help distinguish a
blank capture from a stalled or disabled LCD.

## SameBoy comparison requirements

Use a pinned SameBoy revision and the same ROM hash, CGB hardware revision,
startup/boot policy, initialization assumptions, input and capture anchor. Disable
reference color correction and use comparable raw RGB output for pixel tests.
Define whether a checkpoint means a completed PPU frame or a fixed elapsed
hardware duration; CPU-cycle counts alone are not speed-normalized.

`scripts/sameboy_cgb_capture.c` is a separate public-Core-API adapter for SameBoy
v1.0.3, commit `208ba4afabffab9edde416f2dbb8ae459e34adb8` (the existing audio
reference pin). Build SameBoy's library externally, verify that checkout revision,
then compile the adapter (Linux example):

```sh
make -C /external/SameBoy lib CONF=release
cc -std=c11 -Wall -Wextra -Werror -I/external/SameBoy \
  scripts/sameboy_cgb_capture.c -L/external/SameBoy/build/lib \
  -Wl,-rpath,/external/SameBoy/build/lib -lsameboy -lm -ldl \
  -o /external/sameboy-cgb-capture
python3 scripts/build_dmg_boot_rom.py --model cgb --output /external/cgb-boot.bin
/external/sameboy-cgb-capture /external/gbvisualizer-built/visualizer.gbc \
  /external/cgb-boot.bin 600 /external/sameboy-600.ppm cgb-e 200000000
```

Use GBB's `--dmg-boot` as shown above: despite its historic name, it selects the
bundled replacement boot for CGB too. The exported original GBB fast CGB boot
is the same image (SHA-256
`c8937124cd7dcafc036dcad3ad46008ea57b4e76abfa3da6012ff8bdbd7687d0`).
Record the GBB revision and adapter/library build provenance with local results.

The adapter supports `cgb-e` (default), `cgb-c` and `cgb0`; it fixes random seed 0,
disables color correction and borders, and writes raw 160×144 RGB PPM. It counts
only normal VBlank callbacks, starting from boot; artificial LCD-off/repeat
callbacks are reported separately. Its watchdog uses SameBoy's **8 MHz clocks**,
not GBB CPU T-cycles. A watchdog failure returns 1 without writing a capture;
argument errors return 2 and load/output errors return 4. Existing output files
are never overwritten. Successful metadata is JSON on stdout; Core logs go to
stderr. The existing SGB adapter is unchanged.
An optional final positional argument after the clock budget writes exactly 8 KiB
of CPU-visible WRAM (`C000`–`DFFF`) using read-only public API accesses. The new
snapshot is created exclusively; if it fails, the new PPM is removed and exit 4
is returned without success metadata. An existing snapshot is preserved. Omitting
this argument keeps the original PPM and JSON behavior.

Run the offline contracts, or additionally exercise the compiled adapter using
only an independently authored synthetic ROM and GBB's owned boot:

```sh
python3 tests/gbvisualizer_reference_tools_tests.py
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/gbvisualizer_reference_tools_tests.py
```

These check adaptation, provenance rejection, fail-closed hash validation,
deterministic capture, normal versus artificial frame counting, LCD-off timeout,
argument/load errors and preservation of existing captures. Public CTest registers
the offline contract; native tests are opt-in and require no upstream demo assets.

Record startup differences rather than silently shifting frames until screenshots
match. An upstream example screenshot is useful for orientation, not an exact
frame-600 oracle.

## Initial local evidence (2026-10-16)

Using the pins above, GBB base revision `0229245`, CGB-E and the shared owned
fast boot, two runs per emulator at each checkpoint produced identical RGB bytes
within that emulator. Every capture had four distinct colors, and both displays
changed across checkpoints. Exact cross-emulator comparisons differed:

| Completed PPU frames | Different pixels (of 23,040) | SameBoy 8 MHz clocks | Artificial callbacks |
| --- | --- | --- | --- |
| 60 | 2,160 | 17,352,412 | 65 |
| 300 | 2,520 | 51,059,932 | 65 |
| 600 | 3,914 | 93,194,332 | 65 |

These are local research observations, not redistributed screenshot baselines or
hardware validation. A shared boot image alone does not establish identical
startup timing. Next isolate startup/capture alignment with independently authored
LCD/VBlank and PCM sampling probes before diagnosing the demo's pixel differences
or promoting any output to a required regression gate. No arbitrary frame offset
has been applied.

## Authored frame-anchor probes

`scripts/cgb_frame_anchor_probe.py` builds its own ROM bytes without RGBDS or
external cartridge inputs. Four scenarios combine normal/double speed with
zero/4,096 iterations of an extra LCD-off delay. A VBlank handler increments a
counter and programs the next visible frame's solid red palette marker. Captures
at frames 1, 2, 4 and 8 are checked against the authored marker and each other,
twice per emulator; frame numbers are never shifted. Use the same pinned adapter
and library as above, and rebuild the GBB runner from this checkout so its bundled
boot matches the image exported by the probe:

```sh
python3 scripts/cgb_frame_anchor_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-frame-anchor-results
```

The output directory must be new. The report includes commands, ROM/boot and
executable hashes, reference metadata, RGB hashes, marker results and repeatability.
Record the SameBoy library revision/build separately; the adapter executable hash
alone cannot identify a dynamically linked library. Exit 0 means all authored
markers and paired pixels matched repeatably; exit 1 means a reported difference;
exit 2 means execution, input or metadata failure. Reports always keep
`qualification=false`: these probes do not establish hardware correctness.

Local results with the pins above: all 64 captures were repeatable. In every
scenario GBB frame 1 was RGB `(8,0,0)`, the programmed marker, while SameBoy frame
1 was white `(255,255,255)`. Frames 2, 4 and 8 matched exactly and had the expected
markers `(16,0,0)`, `(33,0,0)` and `(66,0,0)`. The probe therefore exits 1;
the first-frame difference is preserved, not accepted as a passing comparison.
SameBoy checkpoint spacing was 140,448 8 MHz clocks per frame at either CPU speed.
The added delay increased checkpoint times by 229,392 8 MHz clocks in both variants.

Inspection of the pinned SameBoy `Core/display.c`, `GB_display_vblank` and the
LCD-turn-on path, shows an explicit white output while its LCD-turn-on frame-skip
state is active, even for a normal-frame callback. This explains that reference
capture's white first frame; it does not establish which emulator matches hardware.
The later markers argue against a persistent frame-number offset in these probes.
They do **not** explain GBVisualizer's later image differences or establish exact
interrupt/PCM sampling timing. Emulator behavior remains unchanged.

Offline CTest registers `gameboy_cgb_frame_anchor_probe_contract`. An optional
paired native test records the pinned reference's first-frame distinction and
requires subsequent markers and repeatability:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_frame_anchor_probe_tests.py
```

Keep the LCD-turn-on presentation difference as a separate hardware-validation
question rather than offsetting the demo's captures.

## Isolated timer/PCM and STAT/SCX probes

`scripts/cgb_pcm_stat_probe.py` independently authors six ROM scenarios, each in
normal and double speed. They require no assembler, upstream demo code or music:

- Timer reads with powered-on silent channels, CH1 constant-volume pulse, or CH3
  alternating wave samples. The timer reload is 226 with TAC=5, giving 480 CPU
  T-cycles between overflows. The ISR freezes after 64 tuples containing PCM12,
  PCM34, LY and DIV; a completion flag and guard byte catch incomplete or excess
  writes. Authored constraints require the expected channels and amplitudes,
  with both active and inactive samples in the pulse/wave controls.
- A known repeating four-color tile with SCX=0 or SCX=3, plus a mode-2 STAT handler
  setting SCX to LY modulo 8. The pixel oracle is calculated from the authored
  tile, palette and scanline shift. The direct handler writes SCX before mode 3;
  these cases do not yet exercise a late write during pixel transfer.

```sh
python3 scripts/cgb_pcm_stat_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-pcm-stat-results
```

Recompile the adapter with optional WRAM output support before running this tool.
Use the pinned library, a freshly built GBB runner and a new output directory as
above. Each scenario captures frames 2 and 4 twice per emulator: 96 captures in
total. First-frame presentation is deliberately a separate probe, not a frame
offset. GBB's existing frame-state-series supplies WRAM; the adapter's optional
snapshot supplies the matching reference region. No emulator implementation or
timing has changed.

The bounded report records executable, ROM and boot hashes, commands, reference
metadata, recorded samples, sample mismatch indices, authored constraints and
repeatability. PCM cases compare the ordered PCM12/PCM34 values, retaining LY/DIV
as diagnostics; scrolling cases compare RGB pixels against both the authored
oracle and the other emulator. Exit 0 requires all comparisons, constraints and
repeatability to pass; differences return 1 and execution/metadata failures return
2. Reports retain `qualification=false` because agreement with SameBoy does not
establish hardware accuracy. Record the dynamic library build separately.

Local evidence on 2026-10-16 with the same CGB-E/boot/reference pins: all 24 paired
checkpoints passed and all 96 captures were repeatable. PCM sequences matched at
both CPU speeds, including the retained LY/DIV tuples in this run. Frozen sample
buffers were unchanged between frames 2 and 4. All static and per-line scrolling
images matched the authored oracle and SameBoy exactly. These isolated cases
therefore do not reproduce the later GBVisualizer differences. They do not cover
combined timer/STAT/VBlank contention, music updates, noise or arbitrary APU phase.

Offline CTest registers `gameboy_cgb_pcm_stat_probe_contract`; optional paired
native validation is available without external ROM assets:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_pcm_stat_probe_tests.py
```

## Combined timer/STAT/VBlank probes

`scripts/cgb_combined_irq_probe.py` extends the authored pulse and wave controls
with a mode-2 STAT consumer of the timer's sample buffer. It preserves the timer's
HL cursor and maps the selected amplitude to SCX. Its longer handler may write
during mode 3; this differs deliberately from the isolated early-write control.
Two further scenarios reset sampling and retrigger the channel on each VBlank,
alternating pulse amplitude 5/15 or wave amplitude 7/15. The VBlank counter,
completion flag, amplitude constraints and buffer guard validate these handoffs.

```sh
python3 scripts/cgb_combined_irq_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-combined-irq-results
```

Use the same pinned library, adapter and freshly built GBB runner as above.
The four scenarios at both speeds capture frames 2, 3 and 4 twice per emulator:
24 paired checkpoints, 96 captures. The shared bounded harness checks both PCM
sequences and RGB pixels, retaining separate match flags, first sample/pixel
differences and total differing pixels. Combined tuples record PCM12, PCM34, LY
and **TIMA**, replacing DIV at the same instruction cost for finer phase evidence.
TIMA increments every 16 CPU T-cycles here; it is a coarse phase observation, not
an exact instruction timestamp. Reports include phase mismatch indices, commands,
ROM/boot/executable hashes and reference metadata; no frame shifts are applied.

Local evidence on 2026-10-16: every authored completion, guard, channel/amplitude
and VBlank-count check passed, and all captures repeated exactly. However, every
paired checkpoint differed in both PCM samples and pixels. Frame-2 results:

| Scenario | Speed | Different PCM tuples (of 64) | Different pixels (of 23,040) |
| --- | --- | --- | --- |
| Combined pulse | Normal | 19 | 6,560 |
| Combined wave | Normal | 27 | 10,240 |
| VBlank pulse | Normal | 15 | 4,800 |
| VBlank wave | Normal | 25 | 8,480 |
| Combined pulse | Double | 12 | 4,160 |
| Combined wave | Double | 32 | 11,040 |
| VBlank pulse | Double | 3 | 960 |
| VBlank wave | Double | 14 | 4,480 |

In the fixed pulse case, the first normal-speed PCM difference is sample 6; at
double speed it is sample 4. TIMA already differs at sample 1 (GBB 240, SameBoy
241) at either speed, before those PCM differences. This supplies a smaller
reproducer for timer/STAT interaction than the full demo. It does not yet identify
the incorrect emulator or prove that all GBVisualizer differences share this cause.
VBlank updates are unnecessary to reproduce the fixed-channel disagreement.

The research comparison returns **1**, preserves `qualification=false` and retains
all mismatches; it is not a passing reference gate. The automated contract checks
ROM/header validity, handoff constraints, repeatability and truthful reporting.
It intentionally permits a future emulator correction to make the paired images
match rather than requiring today's mismatch forever. Offline CTest registers
`gameboy_cgb_combined_irq_probe_contract`; optional native validation is:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_combined_irq_probe_tests.py
```

## Short IRQ timing probes

`scripts/cgb_irq_timing_probe.py` records 32 events and then disables the timer
and interrupts, freezing the log before capture. Its six authored scenarios are
timer-only, STAT-only, mixed short handlers, mixed with extra STAT work, mixed
with the same-cost SCX write, and IME-disabled/IE=0 STAT polling. The extra-work
control uses three NOPs in place of the 12-T-cycle SCX write. Logging records
source (STAT=2, timer=4), LY, low five IF bits and TIMA. Interrupt handlers observe
acknowledged IF; the polling case observes latched STAT IF without CPU dispatch.

```sh
python3 scripts/cgb_irq_timing_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-irq-timing-results
```

Use the same pinned reference/boot and newly built runner as above. All six cases
run at both speeds, capturing frames 2 and 4 twice per emulator: 24 paired logs,
96 captures. Completion, guard, expected event sources, IF acknowledgement and
polled-request constraints must pass. Reports preserve all events, hashes,
commands, reference metadata and the first differing tuple. Exit 0 requires
repeatable agreement and authored constraints, exit 1 preserves mismatches, and
exit 2 reports execution or input failure. `qualification` remains false.

Local results on 2026-10-16: all guards, event constraints and repeatability checks
passed. Logs froze identically between frames 2 and 4. The matched-cost pad and
SCX controls produced identical logs within each emulator. Six of 24 paired
checkpoints matched; 18 differed. Frame-2 differing event counts (of 32):

| Scenario | Normal speed | Double speed |
| --- | --- | --- |
| Timer only | 0 | 1 |
| STAT only | 0 | 25 |
| Mixed short | 15 | 5 |
| Mixed extra-work NOPs | 16 | 5 |
| Mixed SCX write | 16 | 5 |
| Polled STAT request | 0 | 29 |

At normal speed, the first mixed-short event agrees: `[STAT, LY=1, IF=4, TIMA=232]`,
showing a timer request pending during STAT service. The next event differs:
`[timer, LY=1, IF=0, TIMA=244]` in GBB versus TIMA=245 in SameBoy. The extra-work
and SCX cases likewise first disagree on the following timer event, at TIMA=246
versus 247. Agreement of the pad/SCX controls excludes the SCX register write as
a necessary trigger in these probes. Normal-speed isolated and polled cases do
not reproduce that discrepancy.

Double speed also has independent observations to resolve: timer-only differs
in LY at event 14 (7 versus 8, while TIMA=235 agrees), and the first polled STAT
tuple differs in TIMA (234 versus 230). These must not be folded into a single
unproven explanation. Polling and logging add instruction latency; observations
are quantized by the 16-CPU-T-cycle timer tick and do not establish exact STAT
assertion or interrupt-entry timestamps. No CPU/PPU/APU timing has been changed.

Offline CTest registers `gameboy_cgb_irq_timing_probe_contract`. The optional
native contract validates repeatability, frozen buffers, matched-cost controls
and truthful reporting without requiring today's cross-emulator mismatch forever:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_irq_timing_probe_tests.py
```

The software-request controls below isolate dispatch across `RETI`; the measurements
above describe the pre-correction behavior, not a required mismatch baseline.

## Software requests and the CGB timer-entry correction

`scripts/cgb_reti_dispatch_probe.py` removes hardware STAT assertion and timer
overflow from the dispatch test. It sets requests through IF and uses TIMA as a
stopwatch. Six controls cover a direct call, timer-only interrupt, already-pending
timer after STAT `RETI`, timer raised inside STAT, `EI; RET`, and plain `RET` followed
by re-enabling interrupts in main. Four NOP phases sweep the timer's 16-T-cycle
period in 4-T-cycle steps at both CPU speeds: 48 paired cases, 192 captures.

```sh
python3 scripts/cgb_reti_dispatch_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-reti-dispatch-results
```

Use the same pinned library/adapter and newly built runner as above. Each ROM
records one or two tuples containing source, TIMA, acknowledged IF and a main-code
marker, then disables the timer and interrupts. The marker validates dispatch
order: a pending request after `RETI` runs before main changes it; after plain
`RET`, it runs only after main changes the marker and re-enables IME. Guard and
completion checks catch partial logs. Reports include the first event difference,
provenance and authored timing relations; the completed report is written only
after those relations have been computed.

The controls also check independent instruction-cost relationships:

- Plain `RET` plus main's marker write and `EI/NOP` adds 32 CPU T-cycles over
  `RETI`, or two timer ticks at every phase.
- `EI; RET` adds 4 CPU T-cycles, visible as one extra tick in one of four phases.
- Five NOPs plus CALL cost 44 T-cycles, matching `EI/NOP`, 20-cycle interrupt
  entry and the vector JP. Their first observations should therefore agree.

Before correction, both emulators passed the dispatch-order and RET/EI relative
timing controls, but only 26 of 48 paired cases matched. The direct-call control
matched across emulators at every phase. GBB's timer-only IRQ observation at
phase 0 read TIMA=6, while its equal-cost call and SameBoy both read 7. This
localized a discrepancy before `RETI`, rather than establishing a RETI bug.

Inspection found `Cpu::service_interrupt` applying a DMG timer-boundary exception
to CGB: one 4-cycle entry step advanced other peripherals but skipped the timer.
The condition also affected higher-priority STAT entry when a timer request was
pending. CGB now clocks the timer during all 20 entry cycles; the existing DMG
exception remains unchanged. A focused CPU contract failed before the correction
at all eight tested model/speed combinations. It now checks both timer-only and
mixed-priority requests across CGB0, CGB-C, CGB-E and AGB profiles at both speeds.
The timer-overflow contract separately checks CGB's falling edge and four-cycle
reload delay while preserving the DMG fixture boundary.

After correction, all **48 paired software-request cases match**, all authored
timing relations pass, and captures repeat exactly. Results are identical between
normal and double speed within each emulator. The tool returns 0 for this sweep,
while retaining `qualification=false`: this focused result is not a hardware
qualification of the whole emulator. Offline CTest registers
`gameboy_cgb_reti_dispatch_probe_contract`; optional native validation is:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_reti_dispatch_probe_tests.py
```

The combined timer/STAT/VBlank sweep improves from 0/24 to **12/24 matching
checkpoints**, but remaining PCM and pixel differences stay explicit failures.
The short mixed IRQ logs still differ: after correction, the first normal-speed
mixed-short difference moves from event 1 to event 20, a STAT observation at LY=11
with TIMA=248 versus 247. At double speed its first difference is event 7, a STAT
observation at LY=3 with TIMA=0 versus 255. These are remaining timing boundaries,
not evidence that the software-request correction failed its independent controls.
The external GBVisualizer capture still differs at frames 60, 300 and 600:
2,474, 3,042 and 3,459 pixels respectively in the local post-correction run.
Visual agreement did not improve uniformly; it was not used to select this fix.
No external assets or newly accepted golden screenshots are committed.

## Overflow/reload read boundaries

`scripts/cgb_timer_boundary_probe.py` separates foreground timer reads from HALT
wake-up and interrupt entry, with hardware STAT interrupts disabled. Each owned
ROM starts TIMA at FF and selects TMA=00, E2 or FE, one of four NOP phases, and
either straight-line recording or HALT waiting. Eight tuples record TIMA, IF,
LY and STAT before disabling timer/interrupts and freezing WRAM. Both CPU speeds
are tested: 48 paired cases and 192 captures.

```sh
python3 scripts/cgb_timer_boundary_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-timer-boundary-results
```

Use the same pins and newly built runner as above. The comparison key includes
TIMA and low IF bits; LY/STAT remain explicitly recorded diagnostics. A small
independent timer model predicts bit-3 falling edges, a four-CPU-T-cycle reload
delay and latched IF. The report retains the predicted read times and assumptions:
reads at the end of the bus cycle, HALT wake-up at the next four-cycle sample,
20-cycle interrupt entry, and IF acknowledgement at entry T16. These assumptions
are **not a hardware-qualified bus sampling oracle**. Structural/completion checks
and timing-model agreement are reported separately; missing guards/logs fail closed.

Local results on 2026-10-16: all captures repeated and completed correctly.
All 24 foreground cases matched both the model and SameBoy. Across the full sweep,
46/48 paired cases matched. Both remaining cases use HALT, TMA=FE and phase 0,
one at each CPU speed. At sample indices 1, 3, 5 and 7, GBB reads FE while SameBoy
reads 00; the intervening samples read FF in both. All sampled IF values are 4.
The model places those reads on its reload instants, at 164, 324, 484 and 644
CPU T-cycles after the DIV write. GBB matches its end-of-cycle prediction; SameBoy
does not. That observation does not establish which bus sampling assumption is
correct. The tool returns **1**, retains `qualification=false`, and preserves the
reference disagreement rather than shifting phases to hide it.

The pinned SameBoy `Core/memory.c` TIMA read path explicitly returns zero in its
reloading state. [Pan Docs' timer description](https://github.com/gbdev/pandocs/blob/master/src/Timer_Obscure_Behaviour.md)
documents the delayed reload/IF behavior, while
[Mooneye's `tima_reload` test](https://github.com/Gekkio/mooneye-test-suite/blob/main/acceptance/timer/tima_reload.s)
records hardware-verified TIMA reads on either side of reload, including CGB.
Those references motivate independent validation; the Mooneye test uses a
64-cycle timer and does not itself establish the exact HALT/16-cycle collision
reproduced here. No new hardware run or external conformance run is claimed.
This step changes no CPU, PPU or timer behavior.

Offline CTest registers `gameboy_cgb_timer_boundary_probe_contract`. The optional
native contract validates foreground predictions, repeatability, structural
checks, speed consistency and truthful mismatch reporting; it does not require
either emulator to preserve today's reload-cycle difference forever:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_timer_boundary_probe_tests.py
```

## Pinned external Mooneye timer validation

`scripts/mooneye_timer_reference.py` builds five unmodified external Mooneye timer
tests: `tima_reload`, `tima_write_reloading`, `tma_write_reloading`, `tim01` and
`tim10`. The metadata/ROM hashes are pinned in
`tests/fixtures/external-reference/mooneye-timer.json`:

- Mooneye commit `31510e12eea6286d36eea060a6adde755e1067aa`.
- WLA-DX assembler/linker commit `7ae11df1657dafab1f5fa662889d005709fec15e`.
- SameBoy commit `208ba4afabffab9edde416f2dbb8ae459e34adb8`, as above.

Supply clean checkouts of those exact commits; the tool does not download them.
It builds WLA-DX with CMake Release and the selected ROMs through the upstream
Makefile, placing products outside the source checkouts. Unexpected ROM hashes
fail before emulator execution. Source, ROMs and build products remain external.
Use a fresh GBB runner and recompile the SameBoy adapter from this checkout:

```sh
python3 scripts/mooneye_timer_reference.py \
  --source /external/mooneye-test-suite \
  --assembler-source /external/wla-dx \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/mooneye-timer-results
```

The output directory must be new. CMake, make and a C compiler are needed only
for this opt-in run. `--assembler-build /external/wla-build` can reuse a build
directory; it is reconfigured/rebuilt against the pinned clean source. The
report records source pins, binary/ROM/boot hashes, commands, result output and
repeatability. Record the dynamically linked SameBoy library build separately.
Every test runs twice on both emulators for CGB0, CGB-C and CGB-E: 15 model/test
cases, 30 emulator results, 60 executions. CGB0 uses the exported owned CGB0 boot
(SHA-256 `9120799ac51cabffc521f126cde6789c49f885984f1d7562814eb2bd02273ea6`);
CGB-C/E use the owned CGB boot above, matching GBB's model selection. No Nintendo
boot image is used.

The adapter now accepts `--mooneye ROM BOOT [MODEL] [MAX_8MHZ_CLOCKS]`. It uses
SameBoy's public execution callback to detect `LD B,B` together with the explicit
success registers BC=0305, DE=080D, HL=1522 or failure registers all 42. An ordinary
`LD B,B` without either signature is not a result. JSON includes the model,
result, registers and elapsed 8 MHz clocks. Pass returns 0, explicit test failure
1, argument errors 2, timeout/no progress 3, and load errors 4. No screenshot is
written in protocol mode. Existing capture behavior remains covered by native
tests. The harness rejects unexpected zero-exit output, wrong models/signatures,
missing cases and nonrepeatable results; only complete success returns 0.

Local results on 2026-10-16: all 60 executions passed and repeated identically;
the ROM hashes matched across separate output builds. These are software runs
of upstream hardware-tested cases, not new physical hardware measurements.
The hardware-verified test expectations are documented in the pinned
[Mooneye reload source](https://github.com/Gekkio/mooneye-test-suite/blob/31510e12eea6286d36eea060a6adde755e1067aa/acceptance/timer/tima_reload.s).
The report retains `qualification=false`: passing these tests does not resolve
our exact fast-timer HALT/read collision. `tima_reload` uses a 64-T-cycle timer;
`tim01` checks the fast timer but is not the same reload/read alignment. Both
emulators can pass these controls while disagreeing in the smaller reproducer.
No CPU/timer behavior was changed in this step.

Offline CTest registers `gameboy_mooneye_timer_reference_contract`, checking pin
rejection, hash validation, model-specific boot images, complete coverage and
explicit result parsing without external inputs. The existing optional native
adapter tests exercise authored success/failure/timeout and false-breakpoint ROMs:

```sh
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/gbvisualizer_reference_tools_tests.py
```

## Independent foreground phase matrix

The boundary probe now accepts `--independent-phases`: a foreground-only matrix
with separate 0–3 NOP delays before enabling the timer and before recording reads.
All cases keep IME off and IE zero. Three TMA values × four start phases × four
read phases × two CPU speeds give 96 paired cases and 384 captures. The original
coupled foreground/HALT matrix remains the default; all 48 default ROM images
were checked byte-for-byte against the preceding committed version.

```sh
python3 scripts/cgb_timer_boundary_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-independent-phase-results --independent-phases
```

`--case` runs one case from the selected matrix. A minimal foreground control for
the fast reload slot is:

```sh
python3 scripts/cgb_timer_boundary_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-foreground-reload-control \
  --independent-phases --case poll-fe-start-0-read-1
```

Its first predicted TIMA read is at DIV-relative CPU T-cycle 52, followed by reads
every 80 cycles. The same end-of-bus-cycle model puts indices 1, 3, 5 and 7 on
reload instants at 132, 292, 452 and 612. Both emulators read alternating FF/FE.
The start=2/read=3 control instead starts at cycle 68 and reads FE/FF alternately;
both emulators agree there too. These are not frame offsets or predictions fitted
to SameBoy; the instruction-cost model and new phase cases were defined before
the paired measurements.

Local results on 2026-10-16: **all 96 foreground cases match**, all model predictions
match, and every capture repeats exactly. Normal/double-speed event sequences
agree within each emulator. By contrast, the original HALT/FE/phase-0 control
still reads FE in GBB and 00 in SameBoy at its predicted reload instants. The
foreground experiment therefore did not reproduce the difference, even when
sampling predicted reload slots. This narrows the investigation to HALT wake-up
or hardware-interrupt-entry phase in the tested controls; it does not establish
which emulator is correct or prove that HALT is the sole cause.

The independent report has schema `gbb-cgb-timer-independent-phase-probe-v1`,
separate start/read phase fields and explicit IME-off/IE-zero sampling assumptions.
It retains the same provenance, guard, completion and repeatability checks.
The full foreground run and the minimal case return 0; any paired or predictive
model mismatch still returns 1 with `qualification=false`. An invalid `--case`
returns 2 before creating output. No CPU, PPU or timer behavior changed.

The existing offline boundary contract now checks phase coverage, coupled ROM
equivalence, read-time calculations, case validation and CLI forwarding. Optional
native tests cover the original matrix plus uncoupled reload/control cases without
requiring today's reference mismatch to remain forever.

## HALT, busy waiting and request visibility

`scripts/cgb_halt_wake_probe.py` compares NOP waiting with HALT at a fixed first
timer overflow, with IME either enabled or disabled. TIMA starts at FF, timer
enable completes at predicted DIV-relative T20, overflow is predicted at T32 and
reload/request at T36. HALT begins before the request is pending, avoiding the
HALT bug. IE=4 in these wait controls; with IME off, HALT wakes without vector
entry. The fixed-cost NOP controls reach the same predicted request boundary.
TMA=00 supplies an IF-acknowledgement witness because it cannot overflow again
during recording; TMA=FE supplies the repeated fast reload/read boundary.

```sh
python3 scripts/cgb_halt_wake_probe.py \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-halt-wake-results
```

The same pins, fresh runner and new output-directory requirements apply. Thirty-two
wait/read controls, four IME-off/IE-zero IF-edge controls, and eight DIV-observer
controls run at both speeds: **88 paired cases, 352 captures**. The IF-edge variants
read IF before TIMA directly in foreground code, observing the first request
without HALT or dispatch. The DIV variants replace STAT reads with DIV reads at
the same instruction cost. An observation tuple contains sequential reads, not a
simultaneous snapshot: normally IF follows TIMA by 20 CPU T-cycles and DIV/STAT
by 60; in the IF-edge variants IF precedes TIMA by 20. Predicted times and raw
field order are retained in the report.

Local results on 2026-10-16:

- All capture guards, completion and acknowledgement witnesses passed; every
  result repeated and event sequences agreed between CPU speeds within each core.
- **68/88 paired cases matched.** All IME-disabled wait and IF-edge cases matched;
  the 20 differences were in IME-enabled interrupt-service cases.
- Busy and HALT event logs matched within each emulator for all 64 base wait
  relations. Busy waiting reproduces the IME-enabled difference too, so the
  earlier foreground-only experiment did not establish a HALT-specific cause.
- Both cores read IF=0 at the predicted T32 edge, then IF=4 at T36, T40 and T44.
  This foreground observation agrees on the request's visibility at the tested
  read phases; it does not establish its exact internal acceptance time.
- At read padding 8, the second DIV observation is predicted at T256. GBB reads
  DIV=1 and SameBoy reads DIV=0, for both busy and HALT service and either TMA.
  Read padding 9 adds one NOP and both read DIV=1. The difference therefore
  extends beyond TIMA's special reload value, into the service/read phase seen by
  another register. This brackets a phase boundary; it is not an exact latency
  measurement or a hardware verdict.

The finite model matches GBB's observations; SameBoy differs in the 20 cases
above. Model assumptions are the same next-four-cycle request sample, 20-cycle
entry and end-of-bus-cycle reads used earlier; they remain subject to independent
validation. The report preserves paired differences, predicted/observed sequences,
within-core wait relations, IF/DIV read times, hashes and commands. The comparison
returns **1** with `qualification=false`; no core timing has changed.

Offline CTest registers `gameboy_cgb_halt_wake_probe_contract`. Tests check owned
ROM headers, preservation of the original HALT controls, IME acknowledgement,
IF-edge and DIV calculations, complete logs and truthful failure reporting.
Optional native tests validate repeatability, within-core wait equivalence and
the report's correspondence to observed values; they do not require today's
cross-core disagreement to persist:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_SAMEBOY_CGB_CAPTURE=/external/sameboy-cgb-capture \
  python3 tests/cgb_halt_wake_probe_tests.py
```

Next calibrate timer-driven vector entry and its first instruction with DIV-based
clock witnesses, comparing hardware-requested and software-requested service.
Use pinned interrupt/DIV timing conformance evidence before changing acceptance
or bus sampling; keep STAT arbitration and double-speed LY as separate questions.

### Interrupt/DIV conformance and first-vector clock witnesses

The external Mooneye helper also accepts `--suite interrupt-div`. The default
five-test timer suite remains unchanged. The separate pin file
`tests/fixtures/external-reference/mooneye-interrupt-div.json` selects ten
hardware-verified, CGB-applicable tests at the same Mooneye/WLA-DX revisions:
`div_timing`, `intr_timing`, `reti_intr_timing`, `halt_ime1_timing`,
`halt_ime0_nointr_timing`, `halt_ime0_ei`, `ei_timing`, `ei_sequence`,
`rapid_di_ei`, and `timer/div_write`. Model-restricted `-GS` tests are excluded.
Sources, generated ROMs and raw results stay external; pinned ROM hashes must
match before either emulator runs.

```sh
python3 scripts/mooneye_timer_reference.py --suite interrupt-div \
  --source /external/mooneye-test-suite --assembler-source /external/wla-dx \
  --assembler-build /external/wla-dx-build \
  --runner ./build/gbb_test_runner --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/mooneye-interrupt-div-results
```

On 2026-10-16 all ten tests passed twice in GBB and pinned SameBoy 1.0.3 on
CGB0/CGB-C/CGB-E: **120 successful executions**, with model-matched owned boots.
In upstream `intr_timing`, software serial IF requests bracket the DIV boundary
and check five-machine-cycle interrupt entry. `div_timing` checks DIV reset and
read timing. These independent conformance results support the general clock
and service path; they do not adjudicate a fast hardware timer request's exact
acceptance phase. Reports retain `qualification=false`.

The owned first-vector probe removes the JP-to-recorder from the initial sample:

```sh
python3 scripts/cgb_irq_div_probe.py \
  --runner ./build/gbb_test_runner --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-irq-div-results
```

It runs **96 paired cases / 384 captures** on CGB-E, across both CPU speeds,
six timer-start phases, four vector-read phases and two request sources. IME
is enabled before DIV reset, IE selects only timer, and the CPU waits using NOPs.
The hardware path enables TAC with TIMA=FF/TMA=00. The software path leaves TAC
disabled and writes timer IF at the independently predicted hardware reload
cycle. At vector 0050, zero to three NOPs precede the first `LDH A,(DIV)`; its
result is stored before the recorder samples TIMA and IF. The handler never
returns. Completion, guard and IF-acknowledgement witnesses must pass.

The request alignment and predicted read times assume reload four T-cycles
after overflow, next-four-cycle acceptance, 20-cycle entry and reads at the
end of their bus cycle. They are explicit hypotheses, not measured absolute
hardware timestamps. TIMA is diagnostic only because the software timer is
stopped; paired comparison uses DIV and acknowledged IF.

Local results: **92/96 pairs matched**, every capture repeated, and all structural
witnesses passed. At start phases 43 and 44, with three vector NOPs, the predicted
DIV read is T256. GBB reads DIV=1 for both request sources. SameBoy reads DIV=0
for the hardware request and DIV=1 for software IF, at both speeds. All 48
within-GBB request relations matched; 44/48 SameBoy relations matched. Read
padding two remains below the boundary in both cores. The DIV result is sampled
at the vector itself, so this difference precedes the former recorder JP and
its longer instruction sequence. The full comparison returns **1**, preserving
the unresolved differences, while individual finite-model predictions remain
separate diagnostic fields.

Offline and optional native contracts are in `tests/cgb_irq_div_probe_tests.py`;
CTest registers `gameboy_cgb_irq_div_probe_contract`. Native tests require
`GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE`, check complete repeatable logs and
truthful comparisons, and do not enshrine the current disagreement as expected
hardware behavior.

The optional `--request-phase-sweep` adds software IF requests four CPU T-cycles
before and after the predicted timer request:

```sh
python3 scripts/cgb_irq_div_probe.py --request-phase-sweep \
  --runner ./build/gbb_test_runner --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/cgb-irq-div-request-phase-results
```

This expands the matrix to **192 paired cases / 768 captures**, retaining every
original ROM byte-for-byte. The report uses schema
`gbb-cgb-irq-div-request-phase-probe-v1`. Each capture and request relation records
`software_request_offset_t_cycles` (-4, 0 or +4); predicted request and DIV-read
times include the chosen offset. There are 288 within-core request relations.
Intentionally shifted request relations are diagnostic controls and need not
match. The exit status still requires complete, repeatable cross-core parity
and matching zero-offset timer/software relations; missing cases and genuine
reference differences cannot pass. The original command and matrix remain the
default. Optional native tests now cover this expanded matrix.

On 2026-10-16, **188/192 paired cases matched**. All 768 captures repeated,
all completion/guard/IF witnesses passed, and each scenario's events agreed
between normal and double CPU speed within each core. Only the four original
hardware-timer boundary cases differed across cores; all software-offset
captures matched across cores.

Within each emulator, matching timer/software DIV-and-IF observations were:

| Software IF offset (CPU T-cycles) | GBB | SameBoy 1.0.3 |
| --- | --- | --- |
| -4 | 44/48 | 48/48 |
| 0 | 48/48 | 44/48 |
| +4 | 44/48 | 40/48 |

Thus the earlier software request reproduces every tested SameBoy timer
observation, while the nominal request reproduces every GBB timer observation.
This is a one-machine-cycle observational bracket in this matrix, not an exact
hardware latency measurement: DIV distinguishes only its crossing boundary and
many samples are insensitive to the offset. Predicted request alignment still
assumes the reload and bus-sampling phases described above. Core timing remains
unchanged, `qualification=false`, and the complete comparison returns **1**.

### Hardware-runnable first-vector witness

```sh
python3 scripts/build_cgb_irq_div_hardware.py \
  --output /external/cgb-irq-div-hardware
```

The standard-library-only builder emits eight authored 32-KiB ROM-only CGB
cartridges: timer, software IF at -4/0/+4 CPU T-cycles, each at normal/double
speed. The default start phase 44 and read phase 3 place the disputed DIV read
at the predicted T256 boundary. `--start-phase` and `--read-phase` select other
phases already used by the capture probe. A fresh output directory is required.
`manifest.json` records ROM hashes, generator/measurement-source hashes and
`qualification=false`; no upstream ROMs, fonts, boot firmware or assets are
bundled. The header includes the standard compatibility logo, CGB-only flag,
ROM size/type and valid checksums so an actual CGB boot can accept the cartridge.

Load each `.gbc` on a CGB-compatible flash cartridge. The ROM safely disables the
boot LCD during VBlank, clears CGB BG attributes, then runs the original measured
path. The vector and startup through the request's first 16 NOPs are unchanged;
all new setup precedes the DIV-reset anchor. The valid IRQ path freezes the
original WRAM observations before display code runs. A missing interrupt reaches
a failure continuation after those 16 NOPs instead of looping forever.

The persistent monochrome screen uses independently drawn glyphs and displays:

- Request source, CPU speed (1=normal, 2=double), start/read phase.
- Raw `DIV`, `TIMA`, `IF` and `GUARD` bytes.
- `DONE` when completion/guard/acknowledgement checks pass, otherwise `ERROR`.

Phase and register fields are hexadecimal: start phase 44 appears as `2C`.
IF's reserved upper bits remain visible, so `E0` means no low-five-bit requests
are pending. `GUARD` must be `A5`. DIV values 00 and 01 are both accepted as
measurements; software cases additionally require stopped TIMA=FF. The ROM
exposes the existing Mooneye success/failure register signatures at `LD B,B`
after drawing. **PASS/DONE means the measurement completed structurally; it
never establishes timer/reference parity.** No full trace or cartridge save is
needed, and both raw results remain on screen with IRQs disabled.

Use the existing runner protocol to verify completion, then frame capture to
inspect the screen (the capture mode intentionally does not exit at the protocol
breakpoint):

```sh
./build/gbb_test_runner /external/cgb-irq-div-hardware/timer-start-44-read-3-normal.gbc \
  --model cgb-e --dmg-boot --protocol mooneye --max-cycles 20000000
./build/gbb_test_runner /external/cgb-irq-div-hardware/timer-start-44-read-3-normal.gbc \
  --model cgb-e --dmg-boot --frame-series 4 4 /external/hardware-screen \
  --frame-state-series --max-cycles 20000000
```

Local emulator validation on 2026-10-16 exercised all eight variants in GBB and
pinned SameBoy, with repeated captures, completion signatures, screen-tile/WRAM
correspondence and a deliberately disabled-IRQ failure in both cores. The raw
hardware-wrapper observations matched the original witness in each emulator;
the disputed timer DIV outcome is retained. The native test also verifies the
persistent `ERROR` screen on failure. CTest registers
`gameboy_cgb_irq_div_hardware_contract`; set `GBB_TEST_RUNNER` and
`GBB_SAMEBOY_CGB_CAPTURE` for native checks in
`tests/cgb_irq_div_hardware_tests.py`.

No physical-hardware result has been collected. For independent validation,
record the manifest hash, ROM SHA-256, CGB model/revision, flash cartridge and
loader version, speed/source, and a photograph of each persistent raw result.
Repeat after reset and preserve both observations without choosing an expected
DIV value. Next collect these hardware measurements before changing timer
request acceptance; keep STAT arbitration and double-speed LY separate.


Keep three results separate: GBB's own deterministic screenshot regression,
GBB-versus-SameBoy frame/PCM behavior, and audio waveform comparison. In
particular, the oscilloscope visualizes digital register amplitudes; it does not
measure the analog audio output or replace the existing waveform references.
