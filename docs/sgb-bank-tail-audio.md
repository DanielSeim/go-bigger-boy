# Audible recovery across one-byte semantic upload tails

This gate extends [audible bank recovery](sgb-bank-recovery-audio.md) with single
and repeated semantic rejection when the last physical sample chunk is one
byte. It tests the existing consumed-token synchronization through actual GB
VRAM/SOU_TRN traffic, blocked SOUND commands and a complete changed-bank retry.

Firmware, emulator core, DSP, mixer and state format are unchanged. This is
bounded owned diagnostic evidence; general replacement qualification and
playback remain false. Production still requires private SGB1/SGB2 program
ROMs. No proprietary firmware, instructions, scores, descriptors, samples or
PCM are implementation inputs or committed artifacts. Performance optimization
remains deferred.

## Owned transaction and command sequences

The recovery fixture builder accepts `--kind bad-root --semantic-tail`, with
optional `--repeat`. The defective bank retains the independently authored
fresh triangle sample and invalid zero root. Its physical transactions are:

| Destination | Length | Contents |
| --- | ---: | --- |
| `$2B00` | 2048 | Complete owned score, first root zero |
| `$5000` | 191 | First 191 owned sample-object bytes |
| `$50BF` | 1 | Final owned sample-object byte |
| `$0400` | 0 | Driver handoff |

The uploaded RAM is identical to the preceding whole-object `bad-root`
fixture. Only the physical sample transaction boundary changes. A one-byte
final chunk leaves the IPL jump echo at token 3. On semantic rejection the
host synchronizes its command counter to that consumed token; the next loader
request increments beyond it. The gate observes host WRAM `$23` as 3 at every
rejection and suppressed command, and requires the subsequent uploads to
complete and the final valid bank to be admitted. This is a host-counter
observation, not an independent measurement of SPC port arithmetic.

`tail` retains the preceding eight stages and five SOUND packets. `repeat-tail`
inserts a second defective upload and a blocked SOUND start after the first
blocked start/stop and before the valid retry. It has ten stages and six SOUND
packets. Both reuse the same defective physical payload. The common cartridge
builder now admits at most ten commands; an eleven-command export is rejected.

| Native observation | `tail` | `repeat-tail` |
| --- | ---: | ---: |
| Rejections | 1 | 2 |
| Suppressed SOUND packets | 2 | 3 |
| Rejection/suppression observations | 3 | 5 |
| Complete zeroed score/sample clears | 3 | 4 |
| Valid publications | 2 | 2 |
| Final completed-transfer counter | 3 | 4 |
| Old/fresh note onsets | 2 | 2 |

The completed-transfer counter counts semantic-failure handoffs as well as
valid handoffs. It is not a valid-publication count. Fresh publication must
retain the reversed IDs 10/2 and owned source-3 triangle, with exact score and
sample hashes. The old source-2 square is interrupted only once; neither
rejected upload may create another note or valid publication.

## Replay and acoustic acceptance

The shared native probe accepts `bad-root tail` or `bad-root repeat-tail` after
the voice argument. The native report adds boolean `semantic_tail` and
`repeated_rejection`, plus one consumed-token observation for each rejection or
suppressed SOUND event. Reports retain the shared native schema; the aggregate
checker uses `gbb-sgb-bank-tail-audio-v1`.

Rejection identity includes rejection and suppression counters, completed
transfers, error, blocked state, native readiness/roots, mute/KOF and exact
uploaded RAM hashes. The second semantic rejection must have transfer counter
3, two rejections and two suppressions; its following SOUND must increment
only the suppression counter. The added clear must zero every bounded score
and sample byte before this rejection. The complete retry adds its own clear
and a single valid publication. Native admission must precede fresh SOUND,
retain the final rejection/suppression counts and clear the error/block state.
Premature unblocking before valid publication fails.

Each case requires complete PCM, host state and observations to repeat exactly
after cold reset and in-place save/load, including unread queued output. Replay
masks cover every stage, clear, publication, rejection/suppression event, mute,
note onset/release/zero and admission boundary. Scalar `both` execution must
match complete batched PCM and observations. The existing 100-million-clock,
1536-restore, one-million-PCM-byte, 16-KiB-report and 150-second-child bounds
remain in force.

Four controls change only initial/fresh BRR sample data: `both`, `old`, `new`
and `gb`. The defective middle payload is identical across controls. All
native timelines and complete GB left streams must match exactly. The silent
control's right stream must be zero; whole-stream source residual
`both - old - new + gb` must stay within four units, without alignment or gain
fitting. No clipping is admitted.

Exact right silence spans old release through every blocked command, repeated
upload, retry and admission until fresh restart. Old-only audio cannot return;
new-only audio cannot start early. Early/late old/fresh windows retain the
10-cent pitch limit and distinct waveform hashes. Model-specific GB pulse
windows cover initial clearing, both first blocked commands, final retry
clearing and fresh playback. Repeated rejection adds windows during its own
clear and blocked SOUND. Final stereo tails must be zero.

## CI and reproduction

The complete matrix has 40 cases: two models, two physical voices, two sequence
shapes, four source profiles and scalar `both`. Each case includes reset and
restored execution. CI splits it into complete 20-case tests
`gameboy_sgb_bank_tail_audio_single` and `gameboy_sgb_bank_tail_audio_repeat`,
each with a 3600-second timeout and `sgb-firmware-extended` label. The public
four-method contract remains in platform/sanitizer jobs. The extended set now
contained 72 tests at this milestone across the existing eight dedicated Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_tail_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_tail_audio.py --case repeat-tail \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_recovery_audio_fixture.py --kind bad-root \
  --semantic-tail --repeat --profile both --voice 3 --output /tmp/tail-retry.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_tail_audio_contract$'
```

## Validation evidence

All 40 native cases pass, including eight scalar controls and exact cold-reset
and restored executions. Each capture contains 223492 stereo frames,
955–1079 restores and 472–594 restores with unread output. Every native case
ran fresh in two concurrent local groups. Final checker revalidation verifies
all fixture/PCM hashes, strengthened guards and exact 20-case CI partitions
using those captured outputs; it does not rerun the native cases.

All eight source comparisons have zero residual on both channels and zero
clipped samples. Exact silent recovery gaps span 74863–96663 frames. The 32
bank pitch windows have at least 16 periods and at most 2.821 cents of error;
the 48 GB pulse windows have at least 19 periods and at most 0.120 cents of
error. Maximum period jitter is 0.261 and 0.164 frames respectively. All 160
consumed-token observations are 3. Old-note ENVX at interruption is 65–67;
fresh-note ENVX is 96–98. Native admission follows fresh publication by six
output frames. Every case retains 13 pre-mute DMA gaps with three or four
output frames of advancement.

Seven focused CTests pass: tail, recovery, rejection and stopped/active
replacement contracts, shard runner and firmware build/reproducibility. The
new four-method tail contract passes in 18.48 seconds. It checks exact physical
payloads, source-only control differences, deterministic checksummed exports,
overwrite refusal, command bounds, both sequence shapes/models/voices, and
mutations of consumed tokens, repeated events, transfer/admission counters,
clear contents metadata, event/clear replay coverage, blocked PCM and common
GB dropouts.

The shared probe reproduces the preceding SGB1 voice-2 whole-object `bad-root`
recovery's entire metadata and PCM exactly, including reset/restored execution.
A locally generated owned DA mutation replaces consumed-token synchronization
with a same-length sequence that preserves the old host counter. It fails at
the first semantic rejection with `one-byte semantic tail lost consumed IPL
token`. This checks the new native assertion; it does not independently prove
the later loader's failure mode. The unchanged DA image passes its pinned hash.

CTest discovery assigns the single/repeated matrices to dedicated Linux shards
6 and 5 respectively. The public contract stays unlabelled. Native CLI checks
reject preflight-tail and unknown recovery modes before reading inputs. Probe
build, Python compilation and whitespace checks pass. Earlier complete native
matrices, private references and broad title playback were not rerun.

## Evidence limits and next step

Owned same-renderer controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. Cold rejection before an admitted loop,
combined mixed/one-byte-tail sequences, other semantic defects, command phases,
banks, clocks and presentation rates remain separate work. The DA image
remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

[Mixed-failure recovery](sgb-bank-mixed-audio.md) now covers preflight and semantic
failures in both orders. [Cold rejection recovery](sgb-bank-cold-audio.md) now covers startup without an
earlier valid bank or native note. [Cold one-byte recovery](sgb-bank-cold-tail-audio.md) adds single/repeated
semantic rejection and consumed-token checks before the first valid bank.
[Cold mixed recovery](sgb-bank-cold-mixed-audio.md) covers both orders before the
first valid bank. Next, combine those cold mixed orders with a one-byte semantic
tail, retaining consumed-token, acoustic and replay checks.
