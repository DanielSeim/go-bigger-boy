# Audible active-bank replacement

This gate extends [stopped-bank replacement](sgb-bank-replacement-audio.md) to
SOU_TRN while the old owned loop is still sounding. It uses the same independent
square/triangle banks, reversed instrument map, source-isolation controls and
right-only SNES/left-only GB routing. Firmware, emulator core, DSP, mixer and
state format are unchanged. Production still requires private SGB1/SGB2 program
ROMs; general replacement qualification and playback remain false.

## Active ownership transition

The existing fixture builder accepts `--active`. Its five VBlank delays remain
64, 12, 4, 64 and 12. Stage 2 now only marks a wait: it sends no SOUND stop.
Stage 3 performs the replacement VRAM copy and SOU_TRN. Exactly three SOUND
packets remain: initial start, fresh-bank restart and final stop. Owned score,
sample data, mappings, upload framing and GB pulse behavior are identical to
the stopped fixture. All source controls preserve code and timing, silencing
only BRR sample data.

The old loop must remain active after the upload marker, with positive ENVX
and its loop ENDX bit at the native upload KOF. This KOF must occur within 4096
output frames of stage 3, including the real cartridge transfer and host VRAM
work. The final SOUND stop retains its preceding 960-frame bound. The probe
records the first subsequent DSP mute (`KOF=$FF`, `FLG=$E0`), with output frame,
master clock, APU half-clock and ENVX. KOF, mute, envelope zero and native clear
publication must occur in that order before fresh-bank playback. The observed
zero is a mute/reset transition, not a claim of a normal timed envelope release.

The native A4/A5 observations retain complete bounded score/sample clearing,
readiness invalidation, fixture byte hashes and map checks. Fresh playback
must select source 3, the new triangle and authored 523.251-Hz pitch, after
source 2's 440-Hz square. Both physical voices take this role on both models.

## Observation through DMA

During transfer, a host instruction can include VRAM DMA that advances several
native DSP samples. The instruction-boundary observer cannot inspect every
intermediate ENVX value. Active mode explicitly records each such gap for the
old note: output frame, master clock, before/after native sample counters,
command stage and whether KOF was already observed. These observations are
part of whole-state/reset/save-load equality and must match across controls.

Only pre-KOF gaps in stage 3 are admitted. Each must advance 2..512 native
samples; there may be at most 64, with strictly increasing output frames and
sample ranges. Clock/frame skew is bounded by the observed native advance at
the fixed 48-kHz output rate, plus the existing two-frame observation budget.
Any gap in the new note, after KOF or outside upload fails. Stopped mode still
requires no gaps. This explicitly bounds observation loss; it does not qualify
unobserved intermediate envelope arithmetic. Continuous captured PCM, including
a late old-tone pitch window before interruption, is checked separately.

## PCM and replay acceptance

`check_sgb_bank_replace_audio.py --active` retains the complete four-profile
source comparisons: exact whole left GB streams, identical native timelines,
zero silent-control right output, four-unit stereo source-sum budget, no
clipping, at least 4096 frames of right silence during replacement, no old-only
output after interruption, no premature new-only output, and exact final
stereo silence. Both early and late old/new pitch windows retain the 10-cent
limit; a model-specific GB pulse window is measured during clear/upload.
No alignment, gain fitting or rescaling is applied.

Every native case repeats after cold reset and with exact in-place save/load.
PCM, host state, notes, native gaps, mute, clears and publications must match.
Mute adds both event and unread-output save/load coverage to the preceding
KON/KOF, clear/publication and command-marker coverage. Scalar `both` controls
must match complete batched PCM and observations. Bounds remain 100 million
master clocks, 1536 restores, 1000000 PCM bytes, 16384 metadata bytes and a
150-second child deadline.

The twenty active cases use the same probe and shared contract as stopped
mode. CTest registers `gameboy_sgb_active_bank_replace_audio` separately with a
3600-second timeout and the `sgb-firmware-extended` label. Its full matrix runs
on a dedicated Linux shard; the seven-method public contract exercises both
modes in platform/sanitizer jobs. The extended set had 67 tests at this milestone.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_replace_audio.py --active \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_replace_audio_fixture.py --active \
  --voice 3 --profile both --output /tmp/active-bank.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_replace_audio_contract$'
```

## Validation evidence

All twenty active matrix cases and all four source comparisons pass. Each
captures 223492 stereo frames with zero clipping and exact whole-state, PCM
and observer parity after reset and save/load. Each performs 782..785 restores,
including 309..312 with unread output. Every case covers all command,
KON/KOF, zero, native mute, clearing/publication and required unread-output
restores. All four scalar controls match complete batched results.

Each case records thirteen pre-KOF DMA gaps, advancing three or four native
samples per gap. No post-KOF or new-note gaps occur. Old-note KOF follows stage
3 by 1389..1423 output frames, with ENVX 65..67. The intervening right-channel
silence spans 52136..53317 frames. Whole left GB streams match exactly;
source-sum residual is zero on both channels and final tails are exactly zero.
Sixteen early/late bank pitch measurements have maximum absolute error
2.820511 cents, at least sixteen periods and maximum period deviation 0.242842
frames. GB pulse error is at most 0.119677 cents. All remain below the existing
10-cent limit.

The seven-method public contract passes, including native mute/KOF ordering,
wrong-mode/packet-count rejection, missing/late/oversized/post-KOF DMA gaps,
missing mute restores, different control timelines and late old-tone dropout.
Five focused CTests pass in 17.42 seconds: the shared replacement contract,
transition contract, shard contract and firmware build/reproducibility checks.
The stopped-mode SGB1 voice-2 `both` run reproduces the preceding milestone's
entire metadata and PCM exactly, including reset/restored executions.

The active matrix used native results from this session, retaining temporary
per-case results while running independent voice assignments concurrently.
The successful initial trial was reused after final metadata guards. All
twenty fixture and PCM hashes and native observations were revalidated; the
full checker required all source comparisons and scalar parity. It was not
redundantly rerun through CTest. Probe build, Python compilation, CTest labels,
67-test/eight-shard planning and whitespace checks pass.

## Evidence limits

These owned same-renderer controls establish internal consistency. Independent
DSP arithmetic, hardware timing, private-original acoustics, original samples,
broader title compatibility, other banks/command phases/clocks and presentation
rates remain separate work. No proprietary image, instruction, score, sample
or PCM is an implementation input or committed artifact. The DA firmware image
retains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.
Performance optimization remains deferred.

## Next step

The [rejected-bank audio gate](sgb-bank-rejection-audio.md) adds incomplete and
invalid replacement banks, continued GB audio, no old/fallback sound and blocked
SOUND attempts with reset/queued-output replay. Rejection is recoverable;
playback remains blocked until a valid upload. Next, qualify audible fresh-bank
recovery after those failures.
