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

Next isolate timer-driven PCM sampling and STAT/SCX rendering with authored probes;
keep the LCD-turn-on presentation difference as a separate hardware-validation
question rather than offsetting the demo's captures.

Keep three results separate: GBB's own deterministic screenshot regression,
GBB-versus-SameBoy frame/PCM behavior, and audio waveform comparison. In
particular, the oscilloscope visualizes digital register amplitudes; it does not
measure the analog audio output or replace the existing waveform references.
