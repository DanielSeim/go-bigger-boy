# Audible rejection of replacement banks

This gate extends [active bank replacement](sgb-active-bank-replacement-audio.md)
to an incomplete asset transaction and a complete bank with invalid semantic
contents. An owned right-routed loop is audible before rejection; GB audio
continues on the left. Rejection must mute the old loop, invalidate readiness,
clear old RAM and suppress later SOUND start and stop attempts without old or
fallback PCM. No valid retry is supplied in this gate.

This is owned diagnostic evidence. Firmware, emulator core, DSP, mixer and
state format are unchanged. General qualification and production playback
remain false; production requires private SGB1/SGB2 program ROMs. Performance
optimization remains deferred.

## Owned failures and controls

`build_sgb_bank_reject_audio_fixture.py` reuses the independently authored banks
from the preceding gates. Initial ID 2 selects physical source 2, the calibrated
square loop at 440 Hz. The selected physical voice is 2 or 3; the other rests.
The proposed replacement reverses the map and contains the fresh source-3
triangle and 523.251-Hz score, so it cannot be mistaken for the old bank.

| Failure | Authored defect | Required rejected contents |
| --- | --- | --- |
| `asset-gap` | Asset chunks cover `$5000..5013` and `$5015..50BF`, omitting `$5014` | Preflight prevents transfer; cooperative rejection clears both bounded regions to zero |
| `bad-root` | Complete score/sample upload has first directory root zero | New bytes are present, but no roots/readiness or new-bank publication are admitted |

The same five command markers/delays as active replacement remain: SOUND
start, marker-only wait, bad SOU_TRN, blocked SOUND start and blocked SOUND
stop. The GB pulse remains routed throughout rejection and the first blocked
attempt; only the final marker mutes GB routing. All packets and uploads come
from the GB cartridge through actual VRAM/JOYP behavior.

Two source profiles preserve identical code, score, initial descriptors,
headers and timing. `both` contains the owned initial waveform; `gb` zeros only
its initial BRR data. Both propose the same defective replacement bytes.
Exports are deterministic, checksummed and refuse overwrite. No reference
firmware, score, sample, descriptor or PCM is an implementation input.

## Native failure and replay contract

The shared `gameboy_sgb_bank_replace_audio_probe` accepts either failure name
as its optional mode. Successful stopped/active modes retain their preceding
contracts. Rejected runs require exactly one old-note KON, active ENVX/loop
ENDX at upload KOF, native `KOF=$FF`/`FLG=$E0` mute, and zero envelope before
clear publication. There must be exactly one valid bank publication and two
verified clears; every byte in the bounded score/sample regions is checked at
A4, together with readiness invalidation. Pre-KOF transfer sampling gaps have
the same explicit bounds as active replacement and must match across controls.

Three stable failure observations record initial rejection and each suppressed
SOUND. They include output frame/master clock, rejection/suppression counters,
playback block/error, completed transfer count, A4 phase, all selection/readiness
fields, DSP mute and score/sample hashes. Error is 1 for preflight rejection and
2 for semantic rejection. WRAM `$26` counts completed transfer handoffs, not
admitted banks: it stays 1 after preflight failure and becomes 2 after the
complete but rejected semantic upload. Both retain only the original valid
bank publication. Hashes must match zeros or the exact defective owned bytes.

After stable rejection, every host step must retain blocked status, unadmitted
roots/selection and native DSP mute through the end of capture. Final data
hashes must still match the last suppression event. No fallback, automatic
adoption or readiness escape is allowed. This is recoverable blocked playback,
not a terminal host halt: a future valid SOU_TRN is separately qualified.

Each native case repeats with exact in-place save/load and cold reset. Full
state, PCM, note, gaps, mute, clears/publication and failure events must match.
All three failure/suppression events require both event and unread-output
restore coverage, alongside the preceding command/KON/KOF/zero/mute/clear and
publication coverage. Scalar owned controls must match complete batched
results. Bounds remain 100 million clocks, 1536 restores, 1000000 PCM bytes,
16384 metadata bytes and 150 seconds per child.

## Acoustic acceptance and CI

`check_sgb_bank_reject_audio.py` requires exact native timelines across owned
and silent source controls and byte-identical whole left GB streams. The
silent control's right output must be zero throughout. Both early and late
old-tone windows must measure 440 Hz within the existing 10-cent limit;
the late window ends immediately before upload KOF. Model-specific GB pulse
windows cover initial rejection and the first suppressed SOUND attempt.

All right output must be exactly zero after the old envelope settles, through
rejection, both blocked SOUND attempts and the remaining capture. Final stereo
tails after GB route mute must be exactly zero. Output must have zero clipping;
no shifting, gain fitting or rescaling is applied.

The full matrix has 24 cases: two models, two voices, two failures, each with
owned/silent source controls and scalar owned playback. Each includes reset
and restored executions. `gameboy_sgb_bank_reject_audio` is labelled
`sgb-firmware-extended`, with a 3600-second timeout, for dedicated Linux shards.
The five-method `gameboy_sgb_bank_reject_audio_contract` remains in platform and
sanitizer suites. The extended set contained 68 tests at this milestone.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_reject_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_reject_audio_fixture.py --kind bad-root \
  --voice 3 --profile both --output /tmp/rejected-bank.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_reject_audio_contract$'
```

## Validation evidence

All 24 cases and eight source comparisons pass. Each captures 223492 stereo
frames with zero clipping, exact whole-state/PCM/native-observer equality after
reset and save/load, and complete event/unread-output coverage. Cases perform
737..773 restores, including 275..312 with unread output. All eight scalar
controls match their complete batched results.

Each case records thirteen pre-KOF sampling gaps, advancing three or four
native samples. Upload KOF follows stage 3 by 1389..1423 output frames, with
old ENVX 65..67. Every rejected bank retains the required zero or exact defective
RAM hashes, invalidated readiness and native mute. Both later SOUND attempts
are suppressed 64..67 frames after their markers. There are 143454..145153
frames of exact right silence after the old note settles, including both
attempts and the rest of capture. Whole left GB streams match byte-for-byte;
final stereo tails are exactly zero.

Sixteen old-tone pitch measurements have maximum absolute error 2.334207
cents, at least sixteen periods and maximum period deviation 0.224096 frames.
Sixteen GB pulse windows have maximum error 0.119677 cents, at least nineteen
periods and maximum period deviation 0.160597 frames. All remain below the
existing 10-cent limit.

The five public guard methods pass. They cover exact incomplete/root-invalid
payloads, initial sample-data-only controls, checksums/reproducibility/export
refusal, both failures/models/voices, malformed rejection/readiness/transfer
counts, accidental new note/publication, missing queued restores, invalid mute
or DMA observations, resurrected PCM, late old-tone dropout, GB disturbance
and differing control timelines.

Five focused CTests pass in 17.51 seconds: the new rejection contract, the
preceding stopped/active replacement contract, shard runner and firmware
build/reproducibility. The shared probe also reproduces the committed SGB1
voice-2 successful active-upload run's complete metadata and PCM exactly,
including its reset and restored executions. CTest labels and 68-test/eight-shard
planning, probe build, Python compilation and whitespace checks pass.

The full checker uses native results from this session, retaining temporary
per-case results while running independent failure/model/voice groups
concurrently. The successful semantic trial was reused after native guards;
all 24 fixture/PCM hashes and native observations were revalidated, and all
source comparisons and scalar parity ran through the checker. The matrix was
not redundantly rerun through CTest. Earlier complete acoustic, private
reference and title matrices were not rerun.

## Evidence limits

Owned same-renderer controls establish internal consistency. Independent DSP
arithmetic, physical hardware, private-original acoustics and broad title
compatibility remain unqualified. Other malformed lists/semantic defects,
command phases, clocks, banks and presentation rates remain separate gates.
No proprietary artifacts are committed. The unchanged DA image has SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Next step

The [audible recovery gate](sgb-bank-recovery-audio.md) adds valid changed-bank
retry, fresh source-3 playback, continuous GB audio, silent blocked intervals
and queued replay across admission/restart. The [one-byte semantic tail gate](sgb-bank-tail-audio.md)
also covers single/repeated semantic rejection across consumed-token
synchronization, and [mixed-failure recovery](sgb-bank-mixed-audio.md) covers both
preflight/semantic orders. [Cold rejection recovery](sgb-bank-cold-audio.md) now
covers startup without an earlier admitted bank. [Cold one-byte recovery](sgb-bank-cold-tail-audio.md) adds single/repeated
semantic rejection and consumed-token checks before the first valid bank.
[Cold mixed recovery](sgb-bank-cold-mixed-audio.md) covers both orders before the
first valid bank. [Cold mixed one-byte recovery](sgb-bank-cold-mixed-tail-audio.md) adds exact token
transitions in both failure orders. [Staggered subframe recovery](sgb-bank-cold-phase-audio.md) adds observed LCD/GB
command timing around blocked SOUND and the first fresh restart. Next, cover a
complementary schedule with reversed offsets, retaining acoustic and replay checks.
