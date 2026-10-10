# First audible bank after cold mixed rejection

This gate extends [cold rejection/recovery](sgb-bank-cold-audio.md) to two
failures in opposite orders before the first valid bank: preflight rejection
then semantic rejection, and semantic rejection then preflight rejection.
There is no earlier admitted bank or native note. Firmware, emulator core,
DSP, mixer and state format are unchanged; performance optimization remains
deferred.

All fixtures, scores, descriptors and samples are independently authored.
No proprietary firmware or assets are implementation inputs or committed
artifacts. The replacement program remains experimental and unbundled;
production still requires private SGB1/SGB2 program ROMs. General qualification
and playback remain false in every report.

## Physical uploads and failure order

The owned 32768-byte cartridge contains three physical 4096-byte payloads:
the first defective bank at `$4000`, the opposite defective bank at `$5000`,
and the complete fresh bank at `$6000`. `gap-root` selects `asset-gap` then
`bad-root`; `root-gap` reverses them. The automatic initial SOU_TRN uploads
payload 0 before stage 1. No probe injects commands, RAM writes or DSP state.

The eight actual GB stages retain cold repeated recovery's VBlank delays
64, 16, 4, 4, 16, 4, 64 and 12:

1. Start the GB pulse without SOUND.
2. Attempt blocked SOUND start.
3. Attempt blocked SOUND stop.
4. Upload the opposite defective bank from payload 1.
5. Attempt another blocked SOUND start.
6. Upload the complete valid bank from payload 2.
7. Start the first admitted native note.
8. Stop it and mute GB routing.

Three isolated controls remain `both`, `native` and `gb`. Native-only changes
just the GB routing immediate. GB-only changes just fresh BRR sample data;
code, score, descriptors, maps, BRR headers and both defective payloads remain
identical. Fresh publication has reversed IDs 10/2 and source-3 triangle data.
Deterministic exports require valid checksums and refuse overwrite.

| Observation | `gap-root` | `root-gap` |
| --- | --- | --- |
| Initial error | 1, preflight | 2, semantic |
| Second error | 2, semantic | 1, preflight |
| Initial completed handoffs | 0 | 1 |
| Completed handoffs after second failure | 1 | 1 |
| Initial failed RAM | Zero objects | Exact defective objects |
| Second failed RAM | Exact defective objects | Zero objects |
| Clear generations | 0, 0, 1 | 0, 1, 1 |
| First valid publication generation | 2 | 2 |
| Final rejections / suppressions | 2 / 3 | 2 / 3 |
| Final native notes | 1 | 1 |

Completed-transfer generations count semantic-failure handoffs and valid
handoffs, but exclude preflight rejection. A failed upload must clear readiness
and cannot publish a bank. The second error and exact RAM hashes must replace
the first failure's state; reporting stale counters or hashes fails.

## Native, acoustic and replay contracts

The native probe accepts the first fault followed by `cold-mixed`; reports
require both `cold_rejection` and `mixed_rejection`. The aggregate checker uses
`gbb-sgb-bank-cold-mixed-audio-v1`. It requires five SOUND packets, three full
clears, five failure/suppression observations, one valid publication/admission
and one fresh note. Error, handoff, blocked state, readiness/roots, mute/KOF,
RAM hashes and rejection/suppression counts must match each failure's order.

Publication and admission must follow valid retry and precede fresh SOUND.
Premature unblocking fails. After admission, every host step preserves zero
error, unblocked state and final rejection/suppression counts. The native note
uses the fresh source and pitch, positive ENVX, a real final KOF/zero and the
selected voice's ENDX bit. Every right-channel PCM sample before its first
onset must be zero, including startup, both failures, blocked commands, retry
and admission. Final stereo tails are zero and clipping is rejected.

Whole-stream source isolation and sums use the same renderer without alignment,
rescaling or gain fitting. Native early/late 2048-frame windows require real
sustained first-note pitch. Seven model-specific GB pulse windows cover its
first start, all three suppressed commands, clearing for the opposite defect,
valid retry clearing and fresh playback. Common dropouts in `both` and `gb`
must fail even when source sums still agree.

Each native case compares complete host state, PCM and observations after
cold reset and exact in-place save/load. Event and unread-output replay masks
cover all eight stages, three clears, five failure observations, publication,
admission and note onset/release/zero. Scalar `both` must match complete batched
output. The existing 100-million-clock, 1536-restore, one-million-byte PCM,
16-KiB report and 150-second child limits remain unchanged.

## CI and reproduction

The full matrix has 32 cases: two models, two voices, two orders, three source
profiles and scalar `both`. Each includes reset/restored execution.
`gameboy_sgb_bank_cold_mixed_audio_gap_root` and
`gameboy_sgb_bank_cold_mixed_audio_root_gap` each retain one complete 16-case
matrix, with 3600-second timeouts and the `sgb-firmware-extended` label. The
four-method public guard contract stays in platform/sanitizer jobs. The
extended set now has 80 tests across eight dedicated Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_mixed_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_mixed_audio.py --order root-gap \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_cold_mixed_audio_fixture.py --order root-gap \
  --profile native --voice 3 --output /tmp/cold-mixed.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_cold_mixed_audio_contract$'
```

## Validation evidence

All 32 native cases and eight acoustic comparisons pass, including all eight
scalar controls and exact reset/restored executions. Each capture has 223492
frames, 874–877 restores and 406–410 restores with unread output. Every native
case ran fresh in two concurrent local groups. Final checker revalidation
uses these captures to verify all fixture/PCM hashes and both exact 16-case CI
partitions; it does not rerun the native cases.

All eight whole-stream source residuals are zero on both channels. The 16
native pitch windows contain at least 20 periods with maximum absolute error
2.821 cents; the 56 GB pulse windows contain at least 19 periods with maximum
error 0.956 cents. Native output stays exactly silent until its first note,
for 156351–159853 frames. Every fresh note has ENVX 97, admission follows
publication by five to seven frames and no samples clip. Both failure orders
publish their first valid bank at completed-transfer generation 2.

The four public guard methods pass. They check exact three-payload placement,
source-control differences, deterministic checksummed exports, overwrite
refusal, both models/voices/orders, stale second-failure error/handoff/RAM,
rejection/suppression counts, blocked readiness, clear/publication/admission
generations, missing event/unread-output replay, early native PCM, source
leakage and common GB dropouts throughout the failure sequence.

All eleven focused CTests pass: cold-mixed, cold-tail, cold, warm mixed/tail,
recovery, rejection and replacement contracts, shard runner and firmware
build/reproducibility. The new cold-mixed contract passes in 22.66 seconds.
Fresh SGB1 voice-2 `both` regressions reproduce the preceding cold
`repeat-tail` and warm `root-gap` cases' entire metadata and PCM exactly,
including reset/restored executions. All 64 prior cold and cold-tail captures
still pass the generalized checker with unchanged fixture/PCM hashes. Those
older complete native matrices were not rerun.

CTest discovery assigns the two complete order matrices to dedicated Linux
shards 3/4; the public contract stays unlabelled. Probe build, Python compilation
and whitespace checks pass. Private references, physical hardware and broad
title checks were not rerun.

## Evidence limits and next step

Same-renderer owned controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. These semantic transfers use a whole sample
chunk. Mixed failures with one-byte semantic tails, other semantic defects,
command phases, banks, clocks and presentation rates remain separate work.
The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

Next, combine cold mixed failure orders with a one-byte semantic tail. Require
consumed-token synchronization after semantic failure and the expected token
state through the preflight rejection, preserving first-note silence, fresh
playback, GB continuity and queued-output replay.
