# Owned one-shot completion in combined audio

The [polyphony gate](sgb-polyphony-audio.md) now extends to a naturally ending
owned transient beside a looping SNES voice and active GB pulse at 48 kHz.
Actual audible completion and four onsets (three retriggers) must agree with terminal BRR ENDX
and envelope zero, before the score releases the note gate.

This remains bounded diagnostic evidence: general replacement qualification and
production playback are false. Production still requires proprietary SGB1/SGB2
program ROMs. No firmware, DSP, mixer, save-state format or emulator-core changes
are needed. Independent DSP arithmetic, hardware, original-bank timbre and title
compatibility are outside this milestone. Performance work remains deferred.

## Fixture and controls

`build_sgb_one_shot_audio_fixture.py` retains the preceding 2048-byte score,
192-byte sample object, instrument map, tempo, notes, pan, volumes, gate profiles,
GB pulse and stop/mute commands. Each physical voice takes the held looping role
in turn. The peer source becomes a four-block, 64-sample, filter-zero/range-eleven
transient with independently authored bipolar blocks of amplitudes 7, 5, 3 and 1.
Three B0 headers precede terminal B1 (end without loop). Its admitted one-shot
mode is set; directory loop equals start and unused slot padding is zero.
The other source retains the calibrated two-block square or triangle loop.

Four profiles retain exactly the same score, cartridge instructions, BRR
headers, descriptors, directory, tuning, payload lengths and note scheduling:
`both`, `voice2`, `voice3`, and `gb`. Inactive sources have only BRR data nibbles
zeroed. Their native envelopes and end events still run. SNES remains hard right
and the constant GB pulse hard left. Captures are never shifted or fitted.
Exporters refuse overwrite, and ROM checksums remain reproducible.

## Native completion and acoustic checks

The existing `gameboy_sgb_polyphony_audio_probe` accepts an optional final held
voice argument, 2 or 3, to enable one-shot observations. Its base metadata schema
and existing invocation remain compatible. Extra `completions` entries align
with the five note entries and contain terminal ENDX half-clock/output frame
and natural envelope-zero half-clock/output frame. The held loop has an all-zero
entry. ENDX must first clear after the new KON and the envelope must become
active; a previous note's sticky ENDX cannot count as a fresh completion.
Natural zero must occur before KOF. Gate-off and subsequent zero observations
remain separately recorded, even when the envelope was already zero.

Every execution repeats with exact in-place save/load and cold reset. Full
state, stereo PCM, envelope observer, action/note timeline and natural completion
observations must match. Saves before draining cover all four terminal ENDX and
natural-zero events, both cartridge markers, all five gate-off/zero observations,
and unread output. A scalar control must match the batched `both` run's full PCM and reported
observations exactly.
The bounds remain 55 million master clocks, 90 seconds per child, 1024 restores,
600000 PCM bytes and 8192 metadata bytes.

`check_sgb_one_shot_audio.py` requires each terminal ENDX and natural zero between
KON and KOF, within 768 output frames of KON, with at most 32 frames from ENDX
to zero. These conservative limits cover the short admitted 64-sample object
and native/output pipelines; they do not qualify arbitrary sample lengths.
Each isolated transient must have at least 32 nonzero frames, a peak of at least
100 integer output units, first output within 128 frames, and last output
between 32 and 768 frames after KON. Every retrigger must independently pass.
From natural zero plus 32 frames until four frames before the next KON (or final
KOF), the isolated transient must be exactly silent and the full right mix must
exactly equal the continuing looping control for at least 512 frames.

The full-run source residual `both - voice2 - voice3 + gb` must remain within
the preceding four-unit fixed-point rounding budget on each channel. All four
controls must have identical action/note/completion timelines and sample counts;
the entire left GB stream must be byte-identical. The GB-only right channel
must be exactly zero. No clipping is allowed, and the final stereo tail must
be exactly silent. Held pitch is measured in a 2048-frame window beginning
32 frames after each peer KON, spanning the transient and its natural end.
The existing crossing detector, 10-cent limit and jitter bounds are unchanged.
GB pulse frequency retains its model-specific check.

These source controls share this renderer. Superposition, register/end metadata
and deterministic replay are evidence of internal consistency, not independent
DSP arithmetic or original-firmware acoustic equivalence. Raw PCM and states
stay temporary; the report contains hashes, counters, event times, transient
peaks/durations and pitch measurements. No private samples, instructions, scores,
images or PCM are implementation inputs or committed artifacts. The DA image
remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_polyphony_audio_probe
python3 scripts/check_sgb_one_shot_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_polyphony_audio_probe
python3 scripts/build_sgb_one_shot_audio_fixture.py \
  --held-voice 3 --profile both --output /tmp/one-shot-audio.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_one_shot_audio'
```

The full matrix has twenty runs: both models, both held roles, four source
profiles and one scalar control per group. Its CTest timeout is 2400 seconds.
Public guards reject false ENDX/zero, completion at KOF, incomplete restore
coverage, silent retrigger, stale transient tails, held dropout, GB disturbance,
shifted control timelines and incorrect source sums even when other evidence
remains plausible. Geometry guards cover modes, canonical directory, terminal
headers, padding, data-only isolation, checksums and refusal to overwrite.

## Validation evidence

All twenty runs pass: sixteen batched source controls and four scalar `both`
controls across SGB1/SGB2 and both held roles. Every execution produces 122920
stereo frames, zero clipping, exact whole-state/PCM/observer equality after cold
reset and in-place save/load, and all required lifecycle and natural-completion
coverage. Each replay performs 314..318 restores, including 18..20 with unread
output. Natural ENDX/zero coverage masks are 30 for held voice 2 and 29 for held
voice 3, covering all four peer onsets.

The sixteen isolated transient observations are independently audible, including
all three retriggers in each group. They contain 104..142 nonzero frames and
peak at 126..612 output units. First audible output occurs 11..16 frames after
KON and last audible output 118..155 frames after KON. Terminal ENDX and natural
zero are observed together 161..212 frames after KON (3.35..4.42 ms), while KOF
occurs 2138..2211 frames after KON (44.54..46.06 ms). No gate-release event is
counted as natural completion. All sixteen post-completion windows pass exact
peer silence and equality of the full right mix with the looping control;
these windows span 1891..3924 frames.

All source-control timelines match exactly. Full left GB streams match
byte-for-byte, and maximum whole-run source-sum residual is zero units left and
one right, below the unchanged four-unit budget. All four scalar controls match
full stereo PCM and reported observations, and all final stereo tails are zero.
The sixteen held-pitch windows have maximum error 2.311743 cents, at least
sixteen measured periods and maximum period deviation 0.254622 frames. The
four model-specific GB-pulse measurements have maximum error 0.120091 cents.
Both remain below the unchanged 10-cent limit.

Five public guard methods and nine focused CTests pass (35.81 seconds for the
CTest set), including the preceding polyphony, transition, combined/native and
isolated pitch guards, firmware build/reproducibility and DSP envelopes. The
shared probe builds and both new CTests are registered. A normal and scalar
SGB1/held-voice-2 looping-polyphony run reproduce the preceding milestone's
complete captured report and PCM hashes exactly. New-file whitespace checks,
Python compilation and `git diff --check` pass. The twenty-run matrix was run
through the registered checker directly and was not redundantly rerun via CTest.

No private-original, hardware or independent-DSP execution was performed. The
prior full octave/mapping, broad title and looping-source matrices were not
rerun. This evidence covers the owned four-block transient, two looping peers,
normal mapping, admitted fixed pan/volume/gate points, default clocks and
48-kHz output. Interruption during an audible transient, other block lengths,
broader banks, other presentation rates and title compatibility remain separate.

## Next step

The [audible interruption gate](sgb-one-shot-interruption.md) now covers SOUND
stop/restart during an owned transient with a looping peer and active GB pulse,
including queued-output replay at native interruption and restart. Next, qualify
audible loop/one-shot/loop instrument changes on one voice while its peer and
GB audio continue. Broader banks and title compatibility remain separate.
