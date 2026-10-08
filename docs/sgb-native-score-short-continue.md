# Guarded native short return with ordinary continuation

The native diagnostic now implements the independently authored
[short continuation corpus](sgb-score-short-continue-observations.md).
The returning duration-4 note retriggers its still-keyed channel-2 voice,
releases, then progresses to an ordinary duration-8/16 note or rest at tick 68.
The following note applies deferred ED 64 and starts at actual volume 1/1;
the rest retains actual channel-2 volume 7/7. Both finish with silent audio.
This remains an opt-in direct-RAM/IPL-trampoline diagnostic, with firmware
qualification and production playback false. SGB1/SGB2 program ROMs remain
required.

## Source, admission and bounds

`scripts/build_sgb_score_short_continue.py` derives the preceding short-return
source and adds independently written boundary and completion helpers under
`firmware/sgb/score_short_continue_*.asm`. The reproducible 4081-byte image is
SHA-256 `c0a890f508c30169591ccfcb865113b4198c31af2ef6960fa03256180cbaef70`.
The preceding immediate-final image remains
`d86d1587cb4329b9ded58e42a14d589b66af92fca370badcf3e78c2966139ba0`.

The provisional duration-4 cache slot still requires the full short direct
return profile during silent rehearsal and live initialization. The new
boundary helper admits continuation only when short-return flag C4 is set,
channel 3's continuing duration matches channel 2's known duration 8/16,
its opcode is C9, its articulation matches the return, and both cached tracks
terminate after that event. Existing geometry, masks, initial articulation and
returning-event checks remain mandatory. New subtype flag C5 is cleared at
initialization and on profile misses. BE, C3 and C4 retain their preceding
qualification roles; BA remains zero for ordinary continuation.

Only label-addressed code after the fixed pitch/articulation tables is packed.
The owned four-byte instrument descriptor moves with a label, using the
assembler's absolute indexed label operand. Its SRCN/ADSR1/ADSR2/GAIN values
remain 2/143/111/184. Fixed earlier tables retain their addresses. Program,
source, event, pattern, tick, physical clock, PCM and child execution bounds
are unchanged.

## Physical lifecycle

The returning note retains its bounded five-pulse gate without adding duration
4 to the general note table. Native gates measure 10012..10019 SPC cycles;
original returning gates measure 7426..7433. The existing 4096-cycle
native/reference allowance covers these differences. This does not identify
the original's exact counter or envelope implementation.

Returning and following notes write pitch before actual volume. Both issue
KOF zero, KOF zero, then KON 4, clearing the already released peer's held KOF
bit. Following notes use the existing duration-8/16 pulse table: 10/23 pulses
at articulation 63 and 15/36 at articulation 127. The rest emits no new pitch,
volume or KON. Final completion clears the pending KON mask and reuses the
FF/zero KOF and zero KON sequence without setting final-ready mode.

Owned physical checks retain the positive frozen old envelope and its single
unreleased retrigger. Returning and following notes start new attacks from
zero, reach peak ENVX 127, have real timer releases, and settle. The returning
release precedes the following note; all releases precede final stop. Six
physical timer-release identities remain in rest cases and seven in note
cases. Every physical edge is independently bound to raw DSP writes; inactive
peer KOF assertions and final FF do not invent another release.

Both final tails stay silent for 600000 half cycles. Reset, active save/load,
four tail save/load points, source preservation and cache guards are checked.
The probe exports C5 as `short_continue_mode`; earlier probe modes retain their
existing behavior and schemas.

## Validation and reproduction

```sh
python3 tests/sgb_score_short_continue_playback_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_continue_probe
python3 scripts/check_sgb_score_short_continue_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_continue_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-continue-playback.json
```

The checker also accepts `--reference` with a retained original-only
continuation artifact. The native schema is `gbb-spc-score-short-continue-v1`;
the combined schema is `gbb-score-short-continue-playback-reference-v1`.
Qualification and playback remain false. Public checks need no original
images; the optional local playback-reference CTest requires caller-owned
original images.

All 16 continuation native/original comparisons pass, with maximum raw
SPC-cycle differences:

| Observation | Maximum difference |
| --- | ---: |
| Note gates | 2904 |
| Onset intervals | 2718 |
| Return control offsets | 105 |
| Return voice setup offsets | 445 |
| Following control offsets | 105 |
| Following voice setup offsets | 576 |
| Final control offsets | 139 |
| Unreleased retrigger interval | 271 |
| Final stop interval | 922 |

All 248 preceding comparisons also pass on this image: pending 32, reverse 24,
peer 16, follow-rest 64, final 16, final-peer 16, final-return 32, final-mixed 16,
direct-return 16 and immediate-final short-return 16. No timestamp shifting,
fitted delay, restarted timer or dropped score pulse is used. The intermodel
allowance remains 2048 cycles.

Ten public physical tests cover descriptor relocation/reproducibility,
retrigger/release, deferred mix/existing gates, actual control/setup order,
final silence/reset/save/load, malformed-profile rejection, predecessor
rejection/preceding-profile survival, injected faults and forged reports.
The seven preceding fixture/observation tests remain applicable.
The second-short-event rejection test checks all four new banks.
The implementation milestone passed all 43 public score, fixture and scheduler
CTest suites on the rebuilt
probes. The bundled prototype hash check and `git diff --check` also pass.

Original children retain the 8-million-instruction, 180-second, 16 MiB CSV and
fewer-than-32768-row caps. Private original instructions, scores, instrument
tables, samples and PCM are not inspected or copied. Original ENVX, PCM
equivalence, hardware behavior and real-title qualification are not established.
The bundled prototype remains unchanged at SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.

The [consecutive duration-4 measurements](sgb-score-short-pair-observations.md)
now establish the following short gate and deferred mix behavior. Those banks
remain rejected by this image; guarded admission of a second short event is
the next implementation step.
