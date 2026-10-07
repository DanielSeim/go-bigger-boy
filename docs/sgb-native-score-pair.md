# Experimental native two-track scheduling

The isolated `score_pair.asm` program adds native first-ending-track scheduling
on the independently authored SPC clock. It is not bundled and does not parse
N-SPC phrase tables or render audio. SGB1/SGB2 program ROMs remain required for
production playback.

The diagnostic input at `$2B00` contains exactly eight bytes: two patterns of
`duration2, opcode2, duration3, opcode3`. Each track has one timed event per
pattern. Durations are 1..127; opcodes are notes `$80..C7` or rest `$C9`.
Tempo is supplied at `$12` and supports 96/192, as in the isolated clock.
This format contains no pointers, commands, articulation or end markers.
The SPC validates all eight bytes before emitting any events or starting its
timer. Malformed input rejects with status `$E2`; unsupported tempo uses `$E1`.

Both tracks start at the same score tick. Their remaining durations decrement
independently on each native tick. When either reaches zero, the next pattern
starts immediately, clipping the longer peer. The second pattern follows the
same rule and then disables the timer and reports completion. No host-prepared
timeline drives the native CPU. Five-byte records at `$3000` contain the actual
SPC tick (LE16), raw opcode, duration and channel. Events share logical ticks;
sequential RAM writes are not a claim of simultaneous DSP key-on.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_pair.py --output /tmp/score-pair.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_pair_probe
ctest --test-dir build-dmg-firmware -R 'native_score_pair$' --output-on-failure
```

The builder uses checked clock and event-writer hooks and reproducibly assembles
owned source. The ROM-free suite compares against the independent symbolic
N-SPC scheduler, including the established three owned phrase fixtures:
first-pattern durations `(16,32)`, `(32,16)`, `(32,32)`, followed by `(16,16)`.
It also covers minimum/maximum durations, rests, tempo doubling, malformed input
in both patterns and rejected tempos. The production APU component runs the
native instructions and timers through an owned IPL trampoline with direct RAM
installation. Reset and cross-instance save/load preserve the complete timeline,
including checkpoints after each pattern's event records. PCM remains silent,
DSP mute/reset stays set and no mailbox readiness is published.

The artifact is 283 bytes with SHA-256
`89ebba0a2f780d6158f786e47c913e5751b64c3d742868ca08e891b27763f7dd`.

Optional private black-box comparison:

```sh
python3 scripts/check_sgb_score_pair_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_pair_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

Six fresh original SGB1/SGB2 runs use the owned raw phrase fixtures. The native
first-to-second pattern interval must agree with observed original combined
voice-2/3 key-ons within 4096 SPC cycles, allowing timer quantization and bounded
RAM-log instruction latency. The native experiment emits scheduling records,
not DSP key-ons: this comparison qualifies this bounded transition rule only.
Reports export sanitized register observations and hashes, retain
`qualification: false` and `playback: false`, and include no original code or
samples. These private checks are not reproducible without external images.
The six checked runs agreed within 922 SPC cycles; unequal first patterns
advanced at tick 16 and the equal long pattern at tick 32, all completing after
the final 16-tick pattern.

Next is bounded native phrase-pointer/track-stream parsing, then integration
with the owned two-voice renderer and separate gate/release validation. General
N-SPC execution, live controls, calls, upload/boot and production audio remain
outside this experiment.
