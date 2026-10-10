# Audible recovery after bank rejection

This gate extends [audible rejected uploads](sgb-bank-rejection-audio.md) with
a valid changed-bank retry. An owned source-2 square loop is interrupted by
an incomplete transaction or invalid root, later SOUND start/stop attempts
remain blocked, and a fresh complete bank restores source-3 triangle playback.
GB audio continues through rejection, retry, admission and restart.

This is bounded owned diagnostic evidence. Firmware, emulator core, DSP, mixer
and state format are unchanged. General replacement qualification and playback
remain false; production requires private SGB1/SGB2 program ROMs. No proprietary
firmware, instruction, score, descriptor, sample or PCM is an implementation
input or committed artifact. Performance optimization remains deferred.

## Owned cartridge and controls

`build_sgb_bank_recovery_audio_fixture.py` uses three 4096-byte physical payloads:
the initial calibrated bank, the preceding `asset-gap` or `bad-root` defect, and
a complete fresh bank. Each complete bank has a 2048-byte score and 192-byte
sample object. The fresh bank reverses IDs 2/10 to 10/2, replaces the selected
source with the independently authored triangle, and changes the authored
pitch from 440 to 523.251 Hz. Both physical voices take the active role on
both models. Native envelopes, tuning, right-only routing and volumes retain
the preceding admitted points.

Eight actual GB stages have VBlank delays 64, 12, 4, 16, 4, 4, 64 and 12:

1. Start the owned old loop and left-routed GB pulse.
2. Mark a wait with no SOUND stop.
3. Upload the defective replacement through GB VRAM and SOU_TRN.
4. Attempt SOUND start while blocked.
5. Attempt SOUND stop while blocked.
6. Upload the complete changed bank through GB VRAM and SOU_TRN.
7. Start fresh-bank playback after native admission.
8. Stop fresh playback and mute GB routing.

There are five SOUND packets: one old start, two suppressed attempts, fresh
start and final stop. Both uploads retain the builder's LCD-copy/two-frame
settling sequence. HRAM markers precede real commands; native/output events
anchor measurements. No probe injects packets, RAM writes or DSP state.

Four profiles preserve cartridge code, scores, maps, BRR headers, descriptors
and scheduling. `both` keeps both valid banks audible, `old` silences the fresh
bank's BRR data, `new` silences the initial bank's BRR data, and `gb` silences
both. The defective middle payload is identical in all profiles. Exports are
deterministic, checksummed and refuse overwrite.

## Native admission and replay

The shared `gameboy_sgb_bank_replace_audio_probe` accepts a failure name followed
by `recover`. It retains exactly one rejection and two suppressed SOUND events,
old-note KOF/active ENVX/ENDX, native mute and bounded pre-KOF DMA observations.
Playback must remain blocked with invalid readiness and DSP mute through the
two failed SOUND attempts. The rejection's RAM hashes match zeros for preflight
failure or the exact defective uploaded bytes for semantic failure.

Retry adds a third complete score/sample clear and a second valid publication.
Every byte of both bounded RAM regions must be zero at clear publication;
readiness must be invalidated. Fresh publication must match the valid owned
score/sample hashes and reversed map. The completed-transfer counter ends at
2 after preflight failure or 3 after semantic failure; rejected semantic data
never count as a valid publication.

The first recovery observation records host status/block/error, rejection and
suppression counts, completed transfers, native phase/readiness/roots, DSP mute
and fresh RAM hashes. It must follow valid publication and precede fresh SOUND.
Unblocking before publication fails immediately. After admission, every host
step must retain unblocked status, zero error and unchanged rejection/suppression
counts. Fresh SOUND must produce the second KON on source 3 and a real final
KOF/zero. Final RAM hashes must still match the fresh publication.

Full host state, PCM, observer notes, DMA gaps, mute, three clears, two
publications, three failure events and recovery admission repeat exactly after
cold reset and in-place save/load. All eight marker bits, both KON/KOF/zero
bits, all clear/publication/failure bits and native admission require event and
unread-output replay coverage where applicable. Scalar `both` controls must
match complete batched PCM and observations. Bounds remain 100 million master
clocks, 1536 restores, 1000000 PCM bytes, 16384 metadata bytes and 150 seconds
per native child.

## Acoustic acceptance and CI

`check_sgb_bank_recovery_audio.py` shares the preceding native rejection guards
and whole-source replacement comparisons. It requires exact native/control
timelines and whole left GB streams, zero silent-control right output, zero
clipping and whole stereo residual `both - old - new + gb` within the unchanged
four-unit budget. No alignment, gain fitting or rescaling is used.

The full mix must equal old-only output through the first note's lifetime and
new-only output from fresh onset onward. There must be at least 4096 frames of
exact right silence between old release and fresh onset, covering blocked
commands, retry and admission. Old-only output never returns; new-only output
cannot start early. Early/late 2048-frame windows of both notes retain the
10-cent pitch limit, and the two measured PCM hashes must differ. Model-specific
GB pulse windows cover initial clearing, both blocked SOUND attempts, retry
clearing and fresh playback.
Final stereo tails after stop must be exactly zero.

The full local matrix has 40 cases: two models, two voices, two failure types,
each with four profiles and scalar `both`. Each case includes reset/restored
executions. CI partitions these into complete 20-case tests
`gameboy_sgb_bank_recovery_audio_gap` and `gameboy_sgb_bank_recovery_audio_root`,
using `--kind`, each with a 3600-second timeout and `sgb-firmware-extended` label.
They are assigned to different dedicated Linux shards. The five-method public
contract stays in platform/sanitizer jobs. The extended set had 70 tests at this milestone.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_recovery_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_recovery_audio.py --kind bad-root \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_recovery_audio_fixture.py --kind bad-root \
  --voice 3 --profile both --output /tmp/recovered-bank.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_recovery_audio_contract$'
```

## Validation evidence

All 40 native cases pass, including eight scalar controls and exact cold-reset
and in-place restored executions. Each capture contains 223492 stereo frames,
922–958 restores and 440–476 restores with unread output. The final checker
revalidates every fixture and PCM hash, all strengthened acoustic guards and
the exact 20-case partitions selected by each CI filter. Native cases ran
fresh in two concurrent local groups; cached outputs were used only for this
final checker and partition verification.

All eight source comparisons have zero whole-stream residual on both channels
and zero clipped samples. Exact silent recovery gaps span 74863–76598 frames.
The 32 bank pitch windows have at least 16 periods and at most 2.821 cents of
error; the 40 GB pulse windows have at least 19 periods and at most 0.120 cents
of error. Maximum period jitter is 0.261 and 0.164 frames respectively.
Old-note ENVX at interruption is 65–67; fresh-note ENVX is 96–98. Native
admission follows fresh publication by 6–7 output frames. Every case retains
13 pre-mute DMA gaps with three or four output frames of advancement.

The five public guard methods pass, covering three exact owned payloads and
sample-data-only controls, deterministic checksummed export/overwrite refusal,
both failures/models/voices, stale admission or transfer counts, missing third
clear/publication/replay, residual errors or blocking, premature/late adoption,
wrong fresh source/pitch, silent/brief/wrong-pitched returns, stale old output,
GB disturbance/dropout, timeline differences and source-sum errors.

Six focused CTests pass: recovery, rejection and stopped/active replacement
contracts, shard runner and firmware build/reproducibility. The final recovery
contract, including shared-control GB dropout during a blocked command, passes
in 23.17 seconds. The shared native probe reproduces the preceding SGB1 voice-2
`asset-gap` rejection-only run's entire metadata and PCM exactly, including
reset/restored executions.

CTest discovery assigns the two complete recovery matrices to different Linux
shards; the public contract stays unlabelled. Probe build, Python compilation,
70-test/eight-shard planning and whitespace checks pass. Earlier complete
acoustic, private reference and title matrices were not rerun.

## Evidence limits

Owned same-renderer controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. Other transaction shapes/semantic defects,
repeated or mixed rejection sequences, command phases, banks, clocks and
presentation rates remain separate work. The DA firmware image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Next step

[One-byte semantic upload tails](sgb-bank-tail-audio.md) now add single and
repeated rejection across the consumed-token synchronization boundary. Next,
cover mixed preflight/semantic failures in both orders with silent blocked
intervals, fresh remapped playback, continuous GB audio and queued-output replay.
