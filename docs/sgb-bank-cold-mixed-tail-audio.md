# First audible bank after cold mixed one-byte rejection

This gate combines [cold mixed failures](sgb-bank-cold-mixed-audio.md) with
[one-byte semantic tails](sgb-bank-cold-tail-audio.md). It checks token
synchronization when preflight rejection precedes or follows semantic rejection,
before any valid bank or native note exists. Firmware, emulator core, DSP,
mixer and state format are unchanged. Performance optimization remains deferred.

All fixtures, scores, descriptors and samples are independently authored.
No proprietary firmware or assets are implementation inputs or committed
artifacts. The replacement program remains experimental and unbundled;
production still requires private SGB1/SGB2 program ROMs. General qualification
and playback remain false in every report.

## Physical payload and token transitions

The cold mixed cartridge exporter adds `--semantic-tail`. It preserves the
three physical 4096-byte payloads, actual GB command code, eight stage markers,
VBlank delays, preflight-defective payload and complete fresh bank. Only the
semantic-defective payload's transfer list changes. Its score is followed by
191 sample bytes at `$5000`, a one-byte chunk at `$50BF`, then the `$0400` entry
record. Exact score/sample object bytes are unchanged. Cartridge checksums are
recomputed, exports are deterministic and existing output files are refused.

The semantic payload is at cartridge `$5000` for `gap-root`, or `$4000` for
`root-gap`. The opposite preflight-defective payload remains byte-for-byte
identical to the preceding whole-chunk gate. The valid fresh bank stays at
`$6000`. No probe injects commands, RAM writes or DSP state.

The consumed-token checks follow the owned host recovery protocol. Semantic
failure adopts the IPL's consumed final-jump echo, token `3`. Preflight
rejection sends a clear command by incrementing the current token. Blocked
SOUND packets must not change it:

| Observation | `gap-root` | `root-gap` |
| --- | --- | --- |
| Initial rejection | Preflight, token 1 | Semantic, token 3 |
| First blocked SOUND start | 1 | 3 |
| Blocked SOUND stop | 1 | 3 |
| Opposite rejection | Semantic, token 3 | Preflight, token 4 |
| Second blocked SOUND start | 3 | 4 |

These are five explicit native observation points. A constant list of `3`s
cannot pass either order. Both orders still require one completed handoff
after the two failures and first valid publication at generation 2. Semantic
failure RAM must match the owned defective objects; preflight failure RAM must
be zero. Error, rejection and suppression counters must replace the preceding
failure's state and end at two rejections/three suppressions.

## First audio and lifecycle contracts

The shared native probe adds `cold-mixed-tail`. Reports require
`cold_rejection`, `mixed_rejection`, `semantic_tail`, `repeated_rejection=true`
and the exact five-token integer list for the selected order. The repeated
flag denotes the two-failure sequence. The aggregate checker uses
`gbb-sgb-bank-cold-mixed-tail-audio-v1`.

All prior cold mixed guards remain: five SOUND packets, three complete clears,
five failure/suppression observations, one valid publication/admission and one
fresh source-3 note. Publication/admission must follow valid retry and precede
fresh SOUND; premature unblocking fails. After admission, every host step
preserves zero error, unblocked state and final rejection/suppression counts.
Positive ENVX, fresh pitch, final KOF/zero and selected-voice ENDX are required.
Every native right-channel PCM sample before its first note must be zero.
Final stereo tails must be zero, with no clipping.

The `both`, `native` and `gb` controls isolate fresh native sample and GB pulse
output without changing either defective payload. Whole-stream source sums
require exact source isolation and residuals within four units, without
alignment or gain fitting. Early/late native pitch windows require a sustained
first note. Seven model-specific GB pulse windows cover initial GB start,
blocked commands, both failed/valid retry clear windows and fresh playback.

Every case compares complete host state, PCM and observations after reset and
in-place save/load. Event and unread-output replay masks cover all eight stages,
three clears, five failures, valid publication/admission and note
onset/release/zero. Consumed tokens are included in complete-result equality.
Scalar `both` must match complete batched output. Existing 100-million-clock,
1536-restore, one-million-byte PCM, 16-KiB report and 150-second child bounds
remain unchanged.

## CI and reproduction

The full matrix has 32 cases: two models, two voices, two orders, three source
profiles and scalar `both`. Every case includes reset/restored execution.
`gameboy_sgb_bank_cold_mixed_tail_audio_gap_root` and
`gameboy_sgb_bank_cold_mixed_tail_audio_root_gap` each retain one complete
16-case matrix, with 3600-second timeouts and the `sgb-firmware-extended` label.
The four-method public contract remains in platform/sanitizer jobs. The
extended set had 82 tests at this milestone across eight dedicated Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_mixed_tail_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_mixed_tail_audio.py --order root-gap \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_cold_mixed_audio_fixture.py --order root-gap \
  --semantic-tail --profile native --voice 3 --output /tmp/cold-mixed-tail.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_cold_mixed_tail_audio_contract$'
```

## Validation evidence

All 32 native cases and eight acoustic comparisons pass, including all eight
scalar controls and exact reset/restored executions. Each capture contains
223492 frames, 873–877 restores and 407–411 restores with unread output. Every
native case ran fresh in two concurrent local groups. Final checker
revalidation uses these captures to verify all fixture/PCM hashes and both
exact 16-case CI partitions; it does not rerun native execution.

All five-token lists match `[1,1,1,3,3]` for `gap-root` or `[3,3,3,4,4]` for
`root-gap`. Both orders publish their first valid bank at generation 2. All
eight whole-stream source residuals are zero on both channels. The 16 native
pitch windows contain at least 20 periods with maximum absolute error 2.821
cents; the 56 GB pulse windows contain at least 19 periods with maximum error
0.956 cents. Native output stays exactly silent until its first note, for
156352–159853 frames. Fresh ENVX is 97 in every run; admission follows
publication by five to six frames. No samples clip.

The four public guard methods pass. They check exact one-byte chunk placement
in either physical payload, unchanged preflight/fresh payloads and GB code,
deterministic checksummed exports, overwrite refusal, both models/voices/orders,
strict token types/counts, every token transition point, stale second-failure
state and missing replay. A synchronized token cannot hide wrong error,
handoff, RAM, counters, blocked state, publication or admission.

All twelve focused CTests pass: cold-mixed-tail, cold-mixed, cold-tail, cold,
warm mixed/tail, recovery, rejection and replacement contracts, shard runner
and firmware build/reproducibility. The new contract passes in 10.04 seconds.

Fresh SGB1 voice-2 `both` regressions reproduce the preceding cold
`repeat-tail` and whole-chunk cold `root-gap` cases' entire metadata and PCM
exactly, including reset/restored executions. All 96 prior cold, cold-tail and
cold-mixed captures still pass the generalized checker with unchanged
fixture/PCM hashes. Those older complete native matrices were not rerun.

CTest discovery assigns the two complete order matrices to dedicated Linux
shards 5/6; the public contract stays unlabelled. Probe build, Python compilation
and whitespace checks pass. Private references, physical hardware and broad
title checks were not rerun.

## Evidence limits and next step

Same-renderer owned controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. Other semantic defects, command phases,
banks, clocks and presentation rates remain separate work. The DA image
remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

[Staggered subframe recovery](sgb-bank-cold-phase-audio.md) adds observed LCD/GB
command timing around blocked SOUND and the first fresh restart. Next, cover a
complementary schedule with reversed offsets, retaining acoustic and replay checks.
