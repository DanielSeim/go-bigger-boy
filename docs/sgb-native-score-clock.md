# Experimental native SPC score clock

The [symbolic scheduler](sgb-vendor-score-scheduler.md) now has an isolated,
independently written timing foundation in `firmware/sgb/score_clock.asm`.
It runs on the production SPC CPU, bus, timers and DSP through
`SnesApuAudioEngine`. It does not parse a score, render notes or advertise
mailbox readiness. SGB1/SGB2 program images remain required for production
playback; this experiment is excluded from the bundled prototype.

## Build and clock contract

```sh
python3 scripts/build_sgb_score_clock.py --output /tmp/score-clock.bin
```

The strict existing two-pass assembler produces 79 bytes starting at `$0800`,
SHA-256 `2e8f077e1038e1f53fe6e8acc0f5d82fe652676f6b4815248209007324a85b57`.
The output must not already exist. No binary asset or private firmware is
included in this source build.

The diagnostic caller supplies tempo in direct-page `$12`. Only 96 and 192
are supported. Timer 0 uses target 16 with its 128-cycle prescaler, producing
one pulse per 2048 SPC cycles. Each pulse adds tempo to an eight-bit phase;
carry increments the 16-bit score counter at `$10/$11`. Thus the candidate
model advances tempo/256 score ticks per pulse, retaining the remainder at
`$13`. `$14` is 1 while running or `$E1` for rejected tempo; `$15` holds pending
pulses. These bytes are experimental local state, not a production mailbox.

Startup disables timers, clears counters and phase, and mutes/resets the DSP
with echo writes disabled. It then enables only timer 0. The timer output is
read and cleared, and all observed pending pulses are consumed. Its hardware
output counter is only four bits: this does not recover arbitrary stalls
that overflow that counter. The clock's RAM footprint is reserved only for
this standalone probe; integration must resolve ownership and parser memory.

## Evidence and limits

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_score_clock_probe
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_native_score_clock$' --output-on-failure
python3 scripts/check_sgb_score_clock_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_clock_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The ROM-free probe uses an owned IPL trampoline and directly installs the
owned clock program. It checks 96 ticks per tempo, identical reset timelines,
full component save/load continuation at four physical-half checkpoints,
carry from the low counter byte after 300 ticks, silent queued PCM and zero
mailbox ports. A separate synthetic test advances the bus timer without the
CPU to accumulate three pulses, then checks their consumption and fractional
remainder. That test verifies accumulation logic rather than CPU stall timing.
Empty/oversized images, invalid reports and existing build outputs are rejected.

The optional local reference gate executes six fresh private original-program
cases: baseline, double tempo and short articulation on SGB1/SGB2. It reuses the
owned three-note cartridges and validates the existing reference timing matrix.
Native intervals between ticks 16/32/48 are compared with duration-16 reference
note-onset intervals, with a tolerance of one timer pulse (2048 SPC cycles).
The observed native intervals are 88060/86020 cycles at tempo 96 and
43012/43005 at tempo 192. This constrains a candidate timing primitive for
these fixtures; it does not establish an exact original clock algorithm,
initial note phase, other tempos, articulation gates or audible equivalence.

Reports contain sanitized timing metadata and input hashes. Private ROMs,
sample data and PCM are not exported. Reports explicitly retain
`qualification: false` and `playback: false`. Native parsing, scheduling,
instrument setup and rendering remain required before vendor score playback
can be qualified or bundled.

The [native single-track parser](sgb-native-score-track.md) now exercises this
clock with bounded note/rest streams, separately from bundled playback.
