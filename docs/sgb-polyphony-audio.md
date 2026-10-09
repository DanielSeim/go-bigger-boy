# Owned combined-audio polyphony

The [source-transition gate](sgb-audio-transitions.md) now extends to overlapping
owned SNES voices with active GB audio at 48 kHz. One voice sustains a held note
while its peer plays four independently gated notes. Actual stereo
PCM is compared with source controls on the same observed timeline; correct
register writes alone cannot satisfy the acoustic checks.

This is bounded diagnostic evidence. General replacement qualification and
production playback remain false; proprietary SGB1/SGB2 program ROMs remain
required for production. No firmware, DSP arithmetic, mixer or emulator-core
behavior changes are needed. Hardware, independent DSP arithmetic, original-bank
timbre and title compatibility remain outside this milestone. Performance
optimization remains deferred.

## Owned score and source controls

`build_sgb_polyphony_audio_fixture.py` uploads the preceding three-root,
2048-byte score and 192-byte owned sample object in one 4096-byte physical frame.
Voice 2 uses instrument 2/SRCN 2 and voice 3 uses instrument 10/SRCN 3, both with
normal tuning selector 0. The held role is exercised on each voice. The calibrated
32-sample square and triangle loops, two-block chains and envelopes are unchanged.

The held voice plays authored note 33 (440 Hz convention), duration 64 and
articulation 127. Its peer plays notes 36, 35, 33 and 31 with duration 16 and
articulation 63. Both tracks span 64 logical ticks; the second pattern contains
matching 16-tick rests. Tempo 96 and both gate profiles are already admitted by
the DA engine. The shorter articulation leaves a real silent peer interval
before each retrigger while the held voice continues.

Both SNES voices use the admitted hard-right pan point (E1=0), track volume 127
and song volume 160. A constant volume-4, 50% duty GB pulse at frequency register
`$700` is routed left. After 64 GB frames the cartridge initializes the GB pulse,
writes owned HRAM stage marker 1 and sends SOUND song 1. After another 64 frames
it mutes GB routing, writes marker 2 and sends SOUND stop 128. The score completes
naturally before the final stop. This covers overlap and independent release,
not a new arbitrary stereo/mixing calibration.

Four controls retain identical score bytes, cartridge instructions, BRR headers,
loops, descriptors, tuning and payload lengths:

| Profile | Voice 2 waveform | Voice 3 waveform | GB pulse |
| --- | --- | --- | --- |
| both | Owned square | Owned triangle | Active |
| voice2 | Owned square | Zero sample nibbles | Active |
| voice3 | Zero sample nibbles | Owned triangle | Active |
| gb | Zero sample nibbles | Zero sample nibbles | Active |

Only the sixteen data bytes of each selected two-block BRR chain are zeroed.
No zero score-volume value or unsupported mixing point is introduced. Both
voices remain scheduled, with the same envelopes, KON/KOF events and sample
reads in every profile. A muted control therefore preserves peer behavior.
Using different pan controls was rejected because it changed parser/startup
timing slightly; source data isolation retains exact timing without shifting
or correcting captured PCM. Exporters refuse overwrite.

## Actual PCM contracts

The separate `gameboy_sgb_polyphony_audio_probe` runs the real host for 55 million
master clocks at the default modeled clocks and 48-kHz combined output. Gains
remain the provisional half gain per GB/SNES source. It captures complete stereo
PCM and actual output-frame counters at both cartridge markers and every
observed native key-on, key-off and envelope-zero event. Half-clock observations
remain available separately; native counters are not converted into replacement
output timestamps.

Exactly five notes are required: one held onset and four peer onsets. The first
three peer notes must release and reach zero before their next onset while the
held note remains gated on. Voice/source/pitch identity and completed release
are checked for every note. Envelope setup changes, missed native sample
publications, missing events, clipping and malformed clocks reject.

All controls must retain identical full action and note timelines, frame counts,
GB capture counts and native sample counts. The left GB stream must be
byte-identical across every control for the entire execution. The GB-only
control's right channel must be exactly zero.

For each output frame and channel the checker evaluates:

```
both - voice2 - voice3 + gb
```

Its absolute value must not exceed four integer output units. This conservative
budget covers fixed-point DSP/master-volume rounding and area-resampling/mixer
rounding at the default half gain, without clipping. No samples or timestamps
are aligned, shifted, reconstructed or replaced. Missing or duplicated voices
cannot hide behind the correct pitch registers or a plausible overall PCM hash.
This source superposition check uses this implementation's renderer and is not
an independent DSP arithmetic comparison.

Each peer onset anchors a 2048-frame capture beginning 32 actual output frames
later. The crossing detector excludes the first 128 frames of that capture.
Both isolated peer pitch and held pitch must remain within the preceding
10-cent limit, with at least twelve periods and at most one output sample of
period variation. Captures must finish before either applicable key-off. This
measures eight SNES frequency windows per matched-control group, including the
held voice through every peer retrigger. The GB-only left stream in the `both`
profile supplies a separate model-specific pulse frequency measurement.

For each of the three peer release gaps the checker starts four output frames
after observed envelope zero and stops four frames before the next observed
key-on. At least sixteen frames must remain. The isolated peer's right channel
must be exactly zero; the full mix's right channel must exactly equal the held
control, which must remain audible. This explicitly checks independent release
and continuing held PCM between retriggers. Complete stereo output must be
exactly zero from 3072 frames after the final stop/mute marker through the end.

## Replay and bounds

Every execution repeats with exact in-place host save/load and then cold reset.
Complete host state, full stereo PCM, action edges, observed onset/release/zero
frame counters and envelope observer state must match. Saves occur before
output draining at lifecycle markers, native envelope events, release/zero
observations, 512-frame checkpoints and early frames after cartridge markers.
The probe requires at least one restore with unread PCM, both lifecycle markers
and all five releases and envelope-zero observations in the restore coverage
masks. These caller-owned capture buffers are not save-state format changes.

The matrix covers SGB1/SGB2 and both held-voice roles. Each group runs the four
source profiles and a scalar `both` control: twenty runs in total. Scalar full
stereo PCM, timelines, observer data and reported coverage must match exactly.
The normal/reversed full octave mapping matrix remains separate; this gate uses
the normal two-instrument map only.

Each child retains a 90-second deadline, at most 1024 restores, a 600000-byte
raw PCM limit and an 8192-byte metadata limit. Final reports keep hashes,
frequencies, superposition error, independent-release windows and replay/timeline
metadata, with no raw PCM or save states. Temporary samples are independently
authored. No private images, instructions, samples, scores or PCM are
implementation inputs or committed artifacts. No new private-reference execution
is part of this milestone. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_polyphony_audio_probe
python3 scripts/check_sgb_polyphony_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_polyphony_audio_probe
python3 scripts/build_sgb_polyphony_audio_fixture.py \
  --held-voice 3 --profile both --output /tmp/polyphony-audio.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_polyphony_audio'
```

The full-matrix CTest has a 2400-second timeout. Public guards use synthetic PCM
to test both held roles, the exact four-unit sum boundary, missing/duplicated
sources, GB phase disturbance, stale peer audio, held dropout and wrong pitch
even when superposition still matches. Geometry/export guards check identical
score/code, selective BRR-data muting, checksums, deterministic builds and export
refusal. Malformed replay, queued-output coverage, clocks and incomplete release
metadata reject. Synthetic captures prove parser behavior, not extra whole-host
evidence.

## Validation evidence

All twenty runs pass across both models and both held-voice roles: sixteen
batched source-control runs and four scalar `both` controls. Each execution
emits 122920 actual stereo frames and passes complete cold-reset and restored
state/PCM/timeline/observer equality. Every run performs 312..314 exact in-place
save/load checkpoints, including 18..19 restores with unread output. Both marker
bits and all five release/zero bits are present in every restore coverage mask.
Whole-run mixer clipping counts are zero.

All four source-control groups have identical action and complete note timelines.
Their full left GB PCM streams match byte-for-byte. The maximum full-run
superposition error is zero units on the left and one on the right, below the
unchanged four-unit budget. All four scalar controls exactly match full PCM,
timelines and reported coverage. Final stereo tails are exactly silent.

All twelve independent peer-release gaps pass exact peer silence and equality
between the full right mix and the continuing held control. These windows span
1338..1631 output frames; the held voice remains audible throughout the tested
gaps. The 32 isolated SNES pitch windows have maximum absolute error 2.523930
cents and at least fourteen complete measured periods, with maximum period
deviation 0.254622 samples. Four GB-pulse measurements have maximum error
0.120091 cents. Both sources remain below the unchanged 10-cent limit. No
frequency or sum allowance was expanded after an observed failure.

Six public guard methods pass. Nine focused CTests pass in 55.80 seconds,
covering new polyphony guards, preceding transition/combined/native pitch guards,
isolated owned DSP pitch, real-upload/native-scalar replay, firmware
build/reproducibility and DSP envelopes. The new probe builds, both new CTests
are registered, new-file whitespace checks and `git diff --check` pass. The
full matrix ran directly through the registered checker; it was not redundantly
rerun through CTest.

No new private-original, hardware or independent-DSP comparison was performed.
Earlier complete octave/mapping, transition and broad title matrices were not
rerun. This evidence covers two looping owned instruments, the normal physical
map, admitted fixed pan/volume/gate points, default clocks and 48-kHz output.
One-shot completion, broader banks, other presentation rates and title acoustic
compatibility remain separate work.

## Next step

The [owned one-shot gate](sgb-one-shot-audio.md) now qualifies natural audible
completion and repeated retriggers beside a looping peer and active GB audio.
Next, exercise SOUND stop/restart during an audible transient, including queued
output save/load at interruption and restart. Broader owned sample banks and
real-title acoustic compatibility remain separate work.
