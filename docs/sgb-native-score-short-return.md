# Guarded native duration-4 direct return

The native diagnostic now handles the independently authored duration-4 return
in the [short-return observation corpus](sgb-score-short-return-observations.md).
Channel 2 retriggers its still-keyed voice at tick 64, releases the returning
note, and reaches final readiness at tick 68. The pending final note or rest
retains its independent duration 8 or 16. This is an opt-in direct-RAM/IPL
trampoline diagnostic; firmware qualification and production playback remain
false. SGB1/SGB2 program ROMs remain required.

## Source and admission

`scripts/build_sgb_score_short_return.py` derives the preceding direct-return
source and adds independently written gate, shape and guard helpers under
`firmware/sgb/score_short_return_*.asm`. The reproducible 4089-byte image has
SHA-256 `d86d1587cb4329b9ded58e42a14d589b66af92fca370badcf3e78c2966139ba0`.
The preceding image remains
`11eba7bf28920d59fd0278de5149263fc6803328554d4c030f8cd2e2f998440a`.

The parser provisionally admits duration 4 only for note A0 in cache slot 4,
at cache cursor 80, tempo 96 and articulation 63 or 127. It sets subtype flag
C4 and a five-pulse gate. Rests retain the preceding parser behavior. The
general note-duration table is unchanged.

After parsing, the complete cached score must pass the existing final-return
profile and the new short direct-return shape. C4 requires both final-return
flag BE and direct-return flag C3; an incomplete or continuing score cannot
fall back to ordinary playback. The same guard runs during silent rehearsal
and live initialization. Final-ready flag BA retains its preceding role.
The measured pattern starts, channel masks, controls, articulations, returning
rest, independent pending duration and terminators remain mandatory.

## Gate and physical lifecycle

The five-pulse native gate measures 10013..10020 SPC cycles across the eight
owned banks. The originals measure 7426..7433 cycles. Both articulations use
the same native pulse count, with all differences inside the existing
4096-cycle native/reference allowance. This establishes a bounded diagnostic
profile, not the original's exact counter or envelope implementation. No
timestamp shift, fitted delay, restarted timer or dropped score pulse is used.

Owned DSP checks establish the lifecycle: the old channel-2 envelope stays
positive while channel 3 plays alone, no old-note KOF is invented, the returning
KON starts a new attack from zero and reaches peak ENVX 127, and its real timer
release settles before final readiness. Pitch is written before actual volume;
returning controls remain KOF zero, KOF zero, KON 4. All six timer-release
identities are retained.

The pending A1 changes pitch to 1800 while retaining actual volume 7/7; the
pending rest remains silent. Channel 3 retains actual volume 1/1. Final raw
controls retain KOF FF, KOF zero and KON 4 or zero. Physical release accounting
counts currently keyed voices while retaining every raw DSP write, so final
FF does not invent another release of an already released channel 3.
Owned final-note audio stays held through the bounded 600000-half-cycle tail;
final-rest audio stays silent. Reset, active save/load, four tail save/load
points, source consumption and cached playback are checked.

## Validation and reproduction

```sh
python3 tests/sgb_score_short_return_playback_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_return_probe
python3 scripts/check_sgb_score_short_return_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_return_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-return-playback.json
```

The playback checker also accepts `--reference` with a retained original-only
short-return observation artifact. Its native schema is
`gbb-spc-score-short-return-v1`; its combined result schema is
`gbb-score-short-return-playback-reference-v1`. Both qualification and playback
remain false. The optional local playback-reference CTest requires private
original images and is separate from public tests.

All 16 short-return native/original comparisons pass. Maximum raw differences
in SPC cycles are:

| Observation | Maximum difference |
| --- | ---: |
| Note gates | 2901 |
| Onset intervals | 2840 |
| Final control offsets | 139 |
| Return control offsets | 105 |
| Return voice setup offsets | 445 |
| Boundary pitch to stop | 36 |
| Final stop interval | 3411 |
| Unreleased retrigger interval | 255 |

The new image also passes all 232 retained preceding comparisons: pending 32,
reverse 24, peer 16, follow-rest 64, final 16, final-peer 16, final-return 32,
final-mixed 16 and direct-return 16. The unchanged intermodel allowance is
2048 SPC cycles. Ten public physical tests cover reproducibility,
envelopes, actual setup and controls, final audio, rejection, predecessor
behavior, fault injection and forged observations. The eight preceding short
fixture/observation tests remain applicable. The continuing-profile rejection
test checks all eight new banks. The preceding milestone passed all 41 public
score, fixture and scheduler CTest suites on the rebuilt probes; the subsequent
continuation measurement passes seven focused CTest suites.

Original children retain the 8-million-instruction, 180-second, 16 MiB CSV
and fewer-than-32768-row caps. Native bounds remain 4096 program bytes,
2048 source bytes, four patterns, eight expanded events per track, 64 events,
2032 ticks, 30 million half cycles, 500000 PCM frames and 60 seconds per child.
Private original instructions, scores, instrument tables, samples and PCM are
not inspected or copied. Original ENVX, PCM equivalence, hardware behavior and
real-title qualification are not established by these checks.

The bundled prototype is unchanged at SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.
This profile does not expand production selection or the general note table.

The [continuing-profile measurements](sgb-score-short-continue-observations.md)
now establish the deferred ED control, actual DSP setup and release sequence
outside this immediate-final-ready topology. Their banks remain rejected by
this immediate-final profile; the separate
[native continuation diagnostic](sgb-native-score-short-continue.md) now
implements the measured continuation shape.
