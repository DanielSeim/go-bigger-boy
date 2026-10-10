# First audible bank after cold one-byte rejection

This gate extends [cold rejection/recovery](sgb-bank-cold-audio.md) to semantic
failure after a one-byte final IPL chunk, including a second failed upload
before the first valid bank. It checks consumed-token synchronization, blocked
SOUND, first-bank admission, audible playback and GB continuity. Firmware,
emulator core, DSP, mixer and state format are unchanged; optimization remains
deferred.

All code, scores, descriptors and samples are independently authored owned
fixtures. No proprietary firmware or assets are implementation inputs or
committed artifacts. The replacement remains experimental and unbundled;
production still requires private SGB1/SGB2 program ROMs. Reports retain
`qualification=false` and `playback=false`.

## Physical uploads and command stages

The shared cold fixture accepts `--kind bad-root --semantic-tail`, optionally
`--repeat`. Both cartridges have two physical 4096-byte payloads. The first,
at `$4000`, contains the fresh owned score with its root invalidated and the
complete fresh sample object. Its transfer list has three chunks:

| Destination | Length | Content |
| --- | --- | --- |
| `$2B00` | 2048 | Score with the first two bytes zero |
| `$5000` | 191 | Sample object except its final byte |
| `$50BF` | 1 | Sample object's final byte |

The entry record selects `$0400`. This passes host coverage preflight but fails
driver semantic admission. The consumed IPL token is exactly `3`; the token
must be recorded at every rejection and suppressed SOUND observation. No
failed upload may publish a bank or start a note. The identical failure
payload is shared by all source controls. The complete valid fresh bank at
`$5000` is uploaded only after the failure sequence.

The single sequence retains the cold gate's six stages. The repeated sequence
adds a physical re-upload of payload 0 and another blocked SOUND start:

| Stage | VBlank delay | Actual GB action |
| --- | --- | --- |
| 1 | 64 | Start GB pulse, without SOUND |
| 2 | 16 | Blocked SOUND start |
| 3 | 4 | Blocked SOUND stop |
| 4 | 4 | Repeat defective SOU_TRN from payload 0 |
| 5 | 16 | Blocked SOUND start |
| 6 | 4 | Valid SOU_TRN from payload 1 |
| 7 | 64 | First admitted SOUND start |
| 8 | 12 | SOUND stop and GB routing mute |

The initial defective upload precedes stage 1; no prior valid bank or native
note exists. All commands, VRAM transfers and routing writes execute as actual
GB instructions; the probe injects no commands, RAM writes or DSP state.

The single case has three failure observations, two clears, four SOUND
packets, one rejection and two suppressions. Its first valid publication has
completed-transfer generation 2. The repeated case has five failure
observations, three clears, five SOUND packets, two rejections and three
suppressions. Its first valid publication has generation 3. Rejection and
suppression counters distinguish the repeated failure from a stale error;
failed RAM hashes match the exact owned defective objects. Completed-transfer
generations count semantic-failure handoffs as well as valid handoffs.

## Audio and lifecycle guards

The native probe adds `cold-tail` and `cold-repeat-tail` modes. The checker
requires `cold_rejection`, `semantic_tail`, the exact repeated identity and a
strict integer list of three/five consumed tokens, all `3`. First publication
and admission must follow the complete valid retry and precede its SOUND.
Fresh note source, pitch, positive ENVX, release, ENDX and final zero must match
the owned bank. Native note observations are forbidden before the fresh SOUND
stage, and every right-channel PCM sample before onset must be zero.

The three controls remain `both`, `native` and `gb`. The native-only cartridge
changes just the GB routing immediate. The GB-only cartridge changes just the
fresh BRR data, preserving the initial defective payload. Whole-stream source
isolation and source sums are checked without alignment or gain fitting.
Early/late native pitch windows require a real sustained first note. Five GB
pulse windows cover single failure/retry; the repeated case adds windows
through the second failed upload's clearing and its blocked SOUND. Final stereo
tails must be zero and clipping is rejected.

Every case compares complete host state, PCM and observations after reset and
in-place save/load. Event and unread-output replay masks cover all six/eight
stages, two/three clears, three/five failure observations, first valid
publication/admission and note onset/release/zero. Scalar `both` must exactly
match batched output. Existing 100-million-clock, 1536-restore, 1-million-byte
PCM, 16-KiB report and 150-second child limits remain unchanged.

## CI and reproduction

The full matrix has 32 cases: two models, two voices, two failure sequences,
three source profiles and scalar `both`. Each includes restored and reset
execution. `gameboy_sgb_bank_cold_tail_audio_single` and
`gameboy_sgb_bank_cold_tail_audio_repeat` each run a complete 16-case matrix,
with 3600-second timeouts and the `sgb-firmware-extended` label. The new public
four-method contract remains in platform/sanitizer jobs. At this milestone the
extended set had 78 tests across eight dedicated Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_tail_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_tail_audio.py --case repeat-tail \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_cold_audio_fixture.py --kind bad-root \
  --semantic-tail --repeat --profile native --voice 3 --output /tmp/cold-tail.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_cold_tail_audio_contract$'
```

## Validation evidence

All 32 native cases and eight acoustic comparisons pass, including all eight
scalar controls and exact reset/restored executions. Each capture contains
223492 frames, 786–908 restores and 324–442 restores with unread output. Every
case ran fresh in two concurrent local groups. Final checker revalidation
uses those captures to verify all fixture/PCM hashes and both exact 16-case CI
partitions; it does not rerun the native cases.

All eight whole-stream source residuals are zero on both channels. The 16
native pitch windows contain at least 20 periods with maximum absolute error
2.821 cents; the 48 GB pulse windows contain at least 19 periods with maximum
error 0.956 cents. Native output stays exactly silent until its first note,
for 136757–159853 frames. Every fresh note has ENVX 97, admission follows
publication by six frames and no samples clip. All three/five consumed-token
observations are exactly `3`, including suppressed commands after the second
failed handoff.

The four public guard methods pass, including exact physical chunk placement,
source-control byte differences, deterministic checksummed exports, overwrite
refusal, both models/voices/sequences, strict token identity, missing event and
unread-output replay, stale second-rejection counters/generations/RAM,
premature native output and common GB dropouts during repeated failure/retry.

All ten focused CTests pass: cold-tail, cold, mixed, warm-tail, recovery,
rejection and replacement contracts, shard runner and firmware build/
reproducibility. The new contract passes in 10.30 seconds, including its CLI
export/overwrite checks from CTest's build-directory working directory.

Fresh native SGB1 voice-2 `both` regressions reproduce the preceding cold
`bad-root` and warm `repeat-tail` cases' entire metadata and PCM exactly,
including reset/restored executions. All 32 prior cold captures still pass
the generalized checker with unchanged fixture/PCM hashes; all 32 prior warm
recovery fixture variants are byte-for-byte unchanged by extracting the shared
semantic payload builder. The older full native matrices were not rerun.

CTest discovery assigns the complete repeat/single matrices to dedicated
Linux shards 3/4. The contract stays unlabelled. Probe build, Python compilation
and whitespace checks pass. Private-reference, physical hardware and broad
title checks were not rerun.

## Evidence limits and next step

Same-renderer owned controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified.
[Cold mixed recovery](sgb-bank-cold-mixed-audio.md) adds both failure orders with
whole sample chunks. Other semantic defects, command phases, banks, clocks and
presentation rates remain separate work. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

Next, combine cold mixed failure orders with a one-byte semantic tail, retaining
consumed-token synchronization, silence, fresh playback, GB continuity and replay.
