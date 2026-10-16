# GBVisualizer reference candidate

[LIJI32/GBVisualizer](https://github.com/LIJI32/GBVisualizer) is an external CGB
integration/reference candidate. It is **not currently a registered regression
gate**: no upstream checkout, ROM, music, screenshot baseline or SameBoy capture
has been added, built or validated as part of this documentation change.

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

For a future opt-in run, record the exact upstream commit, RGBDS version, build
command, any local build-only changes, final ROM SHA-256, GBB commit and reference
emulator revision. The [Makefile](https://github.com/LIJI32/GBVisualizer/blob/master/Makefile)
uses RGBDS and an `itt.py` music conversion step; compatibility with the selected
current toolchain must be tested, not assumed. An unidentified ROM or a mutable
branch name is insufficient for a reproducible golden baseline.

## Initial GBB captures

For a locally supplied and pinned `visualizer.gbc`, the existing headless runner
can produce bounded screenshots without SDL:

```sh
./build/gbb_test_runner /path/to/visualizer.gbc --model cgb-e \
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

The existing `scripts/sameboy_sgb_capture.c` selects SGB/SGB2 and is **not** a CGB
reference producer. A dedicated adapter using SameBoy's public Core API, or an
explicitly reviewed CGB extension, is required before this becomes an automated
cross-emulator gate. Record startup differences rather than silently shifting
frames until screenshots match. An upstream example screenshot is useful for
orientation, not an exact frame-600 oracle.

Keep three results separate: GBB's own deterministic screenshot regression,
GBB-versus-SameBoy frame/PCM behavior, and audio waveform comparison. In
particular, the oscilloscope visualizes digital register amplitudes; it does not
measure the analog audio output or replace the existing waveform references.
