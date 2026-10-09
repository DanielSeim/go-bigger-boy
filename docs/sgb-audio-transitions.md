# Combined audio source transitions

The [48-kHz pitch gate](sgb-combined-sample-pitch.md) extends to actual PCM
through SOUND stop/restart and GB routing mute/unmute. Owned controls hold the
GB pulse always on, always silent, or toggle its left-only route while issuing
identical SOUND start/stop commands. The complete SNES right-channel stream must
remain byte-identical across all three profiles, including the transition edges.

This remains diagnostic evidence. Production playback and general replacement
qualification remain false; private SGB1/SGB2 program ROMs remain required for
production. No mixer, DSP arithmetic or firmware behavior changes are needed.
Hardware, independent DSP arithmetic, analog gains and title compatibility are
outside this milestone. Performance optimization remains deferred.

## Owned cartridge and actual events

`build_sgb_audio_transition_fixture.py` uses the preceding two calibrated owned
waves and map. Each case selects one voice, with matching rests on the other.
The first pattern holds authored note 33 (440 Hz convention), duration 64,
articulation 127 and tempo 96, within the existing bounded gate profile. The
three admitted roots and second pattern retain the preceding layout. Stops
interrupt the held first pattern before natural completion.

Six authored stages follow the initial physical SOU_TRN upload:

| Stage | Delay in GB frames | SOUND | GB route in toggle profile |
| --- | --- | --- | --- |
| 1 | 64 | Start song 1 | Left channel 1 on |
| 2 | 8 | No packet | Muted |
| 3 | 4 | No packet | Left channel 1 on |
| 4 | 4 | Stop 128 | Left channel 1 on |
| 5 | 6 | Restart song 1 | Left channel 1 on |
| 6 | 12 | Stop 128 | Muted |

The first stage powers and initializes a constant volume-4, 50% duty GB pulse
at frequency register `$700`. Subsequent actions change only NR51; they never
retrigger or reset its oscillator. The always-on and silent profiles retain the
same instruction count and all SOUND packets, changing only NR51 data values.
Both sources therefore share the same modeled clocks and action timing across
controls. GB-only actions emit no SOUND packet: music zero is outside the
bounded diagnostic mailbox's admitted selection/stop contract.

The common owned cartridge builder accepts optional bounded APU/HRAM writes
before each command, or a write-only action with no packet. It checks register,
value, count and upload bounds. Existing calls without these options keep their
preceding bytes. HRAM `$FF80` records stage 1..6 after the routing write; it is
an owned diagnostic marker, not a production protocol. Export refuses overwrite.

## PCM and lifecycle contracts

The separate `gameboy_sgb_audio_transition_probe` runs the actual host at 48 kHz
for 55 million master clocks. It records the actual drained output counter and
host clock at every observed marker, plus native KON/KOF and envelope-zero
observations. Actual onset output counters anchor each SNES capture: neither a
sent command nor its marker is assumed to be an immediate key-on. The initial
and restarted mailbox paths can delay audible onset by roughly 70 ms.

Each onset must render the correct voice, sample slot and pitch; key-off must
follow its authored stop within 20 ms. Captures contain 2048 output frames after
128 onset warmup frames; the detector excludes another 128 initial frames.
Frequency must remain within the unchanged 10-cent limit, with at least twelve
periods and no more than one sample of period variation. SNES output must be
exactly zero from 1024 frames after the first stop until restart, and from 3072
frames after the last stop through the end of the execution. All output is
checked for hard clipping, with zero mixer clipping required for the whole run.

Silent-GB controls require exact channel equality throughout. In the toggle
profile, left minus right isolates GB audio. Its late mute window, 2816..2943
frames after each mute, must be within two integer output units of zero. The
modeled DMG high-pass coefficient `.999958` per GB clock reduces a full-int16
capacitor offset below one combined-output unit within that interval on either
model with the default half GB gain;
two units cover subtracting separately rounded channels. The immediate modeled
filter tail is retained and is not treated as failed muting.

Four active GB windows retain the model-specific pulse frequency. Comparing
left-minus-right with the always-on control at identical output indices checks
pulse phase after unmute. Routing changes capacitor DC, so equality of raw GB
samples is not required. Instead the first differences of the residual must
stay within four integer units after 512 settling frames: two for differencing
rounded left outputs (the identical right channel cancels) and two for capacitor
offset slew for this bounded volume-4 pulse. A phase shift or duplicated pulse
edge rejects even when measured frequency is unchanged. This is specific to the
owned pulse and is not a general filter or waveform equivalence proof.

Each execution repeats with complete in-place host save/load and then cold
reset. Complete host state, full stereo PCM, action edges and envelope observer
state must match. Restore requests include the six stage boundaries, native
envelope events, 512-output-frame checkpoints and early output frames around
each stage. Saves occur before output draining so queued PCM participates in
restoration. Scalar toggle controls must match full PCM and observed timelines.

The matrix covers both models and two representative paths: voice 2/instrument
2/SRCN 2 and voice 3/instrument 10/SRCN 3, each with the three routing profiles
and a scalar toggle control. Reversed mappings, the full octave and steady-state
pitch retain their preceding separate coverage; this transition matrix does not
repeat all voice/instrument/map combinations.

Each child has a 90-second deadline, at most 1024 restores, a 600000-byte raw PCM
bound and an 8192-byte metadata bound. Temporary raw output contains only owned
samples. The final report retains hashes, frequency statistics, timeline and
replay evidence, without raw PCM or save states. No private ROM instructions,
samples, scores or PCM are implementation inputs or committed artifacts. No new
private-reference execution is part of this milestone. The DA image remains
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_audio_transition_probe
python3 scripts/check_sgb_audio_transitions.py \
  --probe build-dmg-firmware/gameboy_sgb_audio_transition_probe
python3 scripts/build_sgb_audio_transition_fixture.py \
  --profile toggle --output /tmp/audio-transitions.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_audio_transition'
```

The full CTest has an 1800-second timeout. Public guards use synthetic PCM to
exercise phase loss at unchanged frequency, stale GB/SNES output, failed restart,
clipping, wrong pitch, malformed clocks/edges and replay metadata. Fixture guards
check owned geometry, routing, checksums, repeatability, export refusal and IO
bounds. Synthetic captures are parser tests, not extra whole-host evidence.

## Validation evidence

All 16 runs pass: twelve batched source-profile runs and four scalar toggle
controls across both models and both representative voice/instrument paths.
Each run emits 122920 actual stereo output frames, repeats through 328 exact
in-place save/load checkpoints, and passes complete cold-reset state/PCM/edge
and envelope-observer equality. Whole-run clipping counts are zero.

All four matched-control comparisons retain byte-identical complete SNES
right-channel PCM and identical action/onset timelines across the three GB
routing profiles. The stop gap and final SNES tail are exactly silent; both
late GB mute windows meet the two-unit bound. The maximum first-difference
phase residual after GB unmute is three units, below the four-unit bound.
All four scalar controls match complete stereo PCM and observed timelines.

The 32 SNES onset/restart measurements have maximum absolute error 2.185126
cents; the sixteen active GB-pulse measurements have maximum error 0.165078
cents. They contain at least sixteen and nineteen measured periods respectively,
within the unchanged 10-cent frequency tolerance. No observed failure caused
a pitch, filter-tail or phase allowance to be expanded.

Six public guard methods pass. Eight focused CTests pass in 44.68 seconds:
new transition guards, preceding combined/native pitch guards, isolated owned
DSP pitch, real-upload/native-scalar replay, firmware build/reproducibility and
DSP envelopes. A direct comparison with the committed cartridge builder finds
all 110 preceding atomic fixture images unchanged across the valid/fault,
cold, active, stop and third-upload cases. The new probe builds, both CTests are
registered, and `git diff --check` passes. The full matrix ran directly through
the registered checker; it was not redundantly rerun through CTest.

No new private-original, hardware or independent-DSP comparison was performed.
Earlier full octave/mapping and broad firmware/title matrices were not rerun.
This evidence covers the two representative paths, default modeled clocks,
provisional half gains and 48-kHz output. Other rates, GB power-off/reset,
arbitrary waveforms, simultaneous SNES voices and title-level acoustic
compatibility remain outside this milestone.

## Next step

The [owned polyphony audio gate](sgb-polyphony-audio.md) now covers overlapping
SNES voices with active GB audio, independent releases, peer retriggers and
matched source superposition. Next qualify audible one-shot completion and
retrigger with an owned transient sample alongside a looping peer. Broader
sample banks and title-level acoustic compatibility remain separate work.
