# Audible recovery after mixed upload failures

This gate extends [one-byte upload-tail recovery](sgb-bank-tail-audio.md) with
preflight and semantic failures in both orders. An owned active loop is
interrupted, blocked SOUND start/stop attempts follow, a different defective
bank is uploaded, and a complete valid changed-bank retry restores playback.
The gate checks the changing error, handoff count and RAM contents alongside
silence while blocked, fresh playback and continuous GB audio windows.

Firmware, emulator core, DSP, mixer and state format are unchanged. This is
bounded owned diagnostic evidence; general replacement qualification and
playback remain false. Production still requires private SGB1/SGB2 program
ROMs. No proprietary firmware, instructions, scores, descriptors, samples or
PCM are implementation inputs or committed artifacts. Performance optimization
remains deferred.

## Owned cartridge and failure transitions

`build_sgb_bank_mixed_audio_fixture.py` exports a deterministic 32768-byte GB
cartridge with four physical 4096-byte payloads: initial valid bank, first
defective bank, second defective bank and complete fresh bank. The common
cartridge builder now admits at most four payloads, filling `$4000`–`$7FFF`;
a fifth payload is rejected. Code remains below `$4000`. Header/global
checksums and overwrite refusal remain required.

`gap-root` first uses the existing `asset-gap` payload, which is rejected during
host preflight and leaves zeroed score/sample RAM. Its second payload is the
existing complete `bad-root` upload, which reaches the driver but fails semantic
admission and leaves the exact defective RAM contents. `root-gap` reverses
these two physical payloads. The semantic payload uses the whole sample object;
the preceding one-byte-tail boundary remains a separate gate.

Ten actual GB stages retain the repeated-rejection fixture's delays:

1. Start the owned old source-2 square loop and left-routed GB pulse.
2. Mark a wait without stopping the old note.
3. Upload the first defective bank through GB VRAM/SOU_TRN.
4. Attempt blocked SOUND start.
5. Attempt blocked SOUND stop.
6. Upload the other defective bank through GB VRAM/SOU_TRN.
7. Attempt another blocked SOUND start.
8. Upload the complete valid changed bank.
9. Start the fresh source-3 triangle loop.
10. Stop the fresh note and mute GB routing.

The fixture sends six SOUND packets. The three suppressed commands must not
start a note. Neither defective upload may publish a valid bank. All four
profiles (`both`, `old`, `new`, `gb`) retain identical code, scheduling and both
defective payloads; only BRR data in the initial/fresh valid banks change.
Fresh publication must retain reversed IDs 10/2 and exact owned score/sample
hashes. No probe injects commands, RAM writes or DSP state.

| Observation | `gap-root` | `root-gap` |
| --- | --- | --- |
| Error after first failure and its two SOUND attempts | 1 | 2 |
| Error after second failure and its SOUND attempt | 2 | 1 |
| Completed transfers after first failure | 1 | 2 |
| Completed transfers after second failure | 2 | 2 |
| RAM after first failure | Zero score/sample objects | Exact defective objects |
| RAM after second failure | Exact defective objects | Zero score/sample objects |
| Clear generations | 0, 1, 1, 2 | 0, 1, 2, 2 |
| Valid publication generations | 1, 3 | 1, 3 |
| Final completed transfers / rejections / suppressions | 3 / 2 / 3 | 3 / 2 / 3 |

The completed-transfer counter includes the semantic-failure handoff, but not
the preflight failure. It is not a valid-publication count. Repeated clear
generations are intentional: a preflight failure clears RAM without completing
a new handoff.

## Native, replay and acoustic guards

The shared native probe accepts the first failure name (`asset-gap` or
`bad-root`) followed by `mixed`. Its report retains the shared native schema
and adds `mixed_rejection: true`. The aggregate checker uses
`gbb-sgb-bank-mixed-audio-v1` and retains `qualification=false` and
`playback=false`.

Five rejection/suppression observations require exact rejection/suppression
counts, error, completed transfers, blocked state, readiness/roots, DSP mute/KOF
and known RAM hashes. The second rejection must increment only the rejection
count; its following SOUND increments only suppression. Every clear zeros all
2048 score and 192 sample bytes and invalidates readiness. There are four
clears, two valid publications, two note onsets and one observed initial
KOF/mute boundary. The old active note must release and reach zero; fresh SOUND
must use source 3 with its changed pitch and produce a real final KOF/zero.

Valid retry must be completely published and admitted before fresh SOUND.
Unblocking before valid publication fails immediately; after admission, every
host step must preserve unblocked status, zero error and final rejection/
suppression counts. Final score/sample RAM must match fresh publication.

Each native case compares complete host state, PCM and observations after
cold reset and in-place save/load. Event and unread-output replay masks must
cover all ten stages, both notes, four clears, both publications, all five
failure events, mute and recovery admission. Scalar `both` must match complete
batched PCM and observations. The existing 100-million-clock, 1536-restore,
one-million-PCM-byte, 16-KiB-report and 150-second-child bounds remain in force.

Four source controls require identical native timelines and complete GB left
streams, zero silent-control right output, no clipping and whole stereo source
residual `both - old - new + gb` within four units. No alignment, gain fitting
or rescaling is used. Exact right silence spans old release, both rejected
uploads, all blocked commands, retry and admission until fresh restart.
Old-only output cannot return; new-only output cannot start early. Early/late
old/fresh windows retain the 10-cent pitch limit and distinct waveform hashes.
Seven model-specific GB pulse windows cover initial clearing, all three blocked
commands, second-failure clearing, retry clearing and fresh playback. Final
stereo tails must be zero.

## CI and reproduction

The full matrix has 40 cases: two models, two physical voices, two failure
orders, four source profiles and scalar `both`. Each case includes reset and
restored execution. CI partitions it into complete 20-case tests
`gameboy_sgb_bank_mixed_audio_gap_root` and
`gameboy_sgb_bank_mixed_audio_root_gap`, each with a 3600-second timeout and
`sgb-firmware-extended` label. They run on different dedicated Linux shards.
The public four-method contract remains in platform/sanitizer jobs. The
extended set contained 74 tests at this milestone across the existing eight Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_mixed_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_mixed_audio.py --order root-gap \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_mixed_audio_fixture.py --order gap-root \
  --profile both --voice 3 --output /tmp/mixed-retry.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_mixed_audio_contract$'
```

## Validation evidence

All 40 native cases pass, including eight scalar controls and exact cold-reset
and restored executions. Each capture contains 223492 stereo frames,
1044–1048 restores and 557–562 restores with unread output. Every native case
ran fresh in two concurrent local groups. Final checker revalidation verifies
all fixture/PCM hashes, acoustic guards and the exact 20-case CI partitions
using those captured outputs; it does not rerun the native cases.

All eight source comparisons have zero residual on both channels and zero
clipped samples. Exact silent recovery gaps span 94457–96665 frames. The 32
bank pitch windows have at least 16 periods and at most 2.821 cents of error;
the 56 GB pulse windows have at least 19 periods and at most 0.120 cents of
error. Maximum period jitter is 0.261 and 0.164 frames respectively. Old-note
ENVX at interruption is 65–67; fresh-note ENVX is 96–97. Native admission
follows fresh publication by 6–7 output frames. Every case retains 13 pre-mute
DMA gaps with three or four output frames of advancement. All observed error,
handoff and clear-generation sequences match the table above.

The four public guard methods pass. They check exact four-payload placement,
source-only control differences, deterministic checksummed export, overwrite
refusal, payload bounds, both orders/models/voices, stale error/RAM state,
wrong completed-transfer counts, incomplete clears or admission, missing
failure/clear replay, blocked PCM and shared GB dropouts.

Eight focused CTests pass: mixed, tail, recovery, rejection and stopped/active
replacement contracts, shard runner and firmware build/reproducibility. The
mixed contract passes in 28.56 seconds. The shared native probe reproduces the
preceding SGB1 voice-2 repeated semantic-tail recovery's entire metadata and
PCM exactly, including reset/restored executions.

CTest discovery assigns the two complete order matrices to dedicated Linux
shards 1 and 2. The public contract stays unlabelled. Probe build, Python
compilation and whitespace checks pass. Earlier complete native matrices,
private references and broad title playback were not rerun.

## Evidence limits and next step

Owned same-renderer controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. Longer/more varied failure sequences, combined mixed/one-byte-tail uploads,
other semantic defects, command phases, banks, clocks and presentation rates
remain separate work. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

[Cold rejection recovery](sgb-bank-cold-audio.md) now covers startup without an
earlier valid bank or native note. [Cold one-byte recovery](sgb-bank-cold-tail-audio.md) adds single/repeated
semantic rejection and consumed-token checks before the first valid bank.
[Cold mixed recovery](sgb-bank-cold-mixed-audio.md) covers both orders before the
first valid bank. [Cold mixed one-byte recovery](sgb-bank-cold-mixed-tail-audio.md) adds exact token
transitions in both failure orders. [Staggered subframe recovery](sgb-bank-cold-phase-audio.md) adds observed LCD/GB
command timing around blocked SOUND and the first fresh restart. Next, cover a
complementary schedule with reversed offsets, retaining acoustic and replay checks.
