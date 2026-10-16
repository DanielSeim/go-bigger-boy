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

Next isolate dispatch of an already-pending timer request immediately after STAT
`RETI` at normal speed with a controlled software-request timing probe. Investigate
double-speed LY/STAT boundaries separately. Establish an independent timing
contract before changing emulator behavior.

Keep three results separate: GBB's own deterministic screenshot regression,
GBB-versus-SameBoy frame/PCM behavior, and audio waveform comparison. In
particular, the oscilloscope visualizes digital register amplitudes; it does not
measure the analog audio output or replace the existing waveform references.
