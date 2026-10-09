# Owned sample pitch calibration

The independently authored stress BRR assets used by earlier diagnostics are
not a calibrated instrument bank. DSP pitch-register agreement does not imply
a matching audible fundamental: waveform period, predictor history and loop
geometry also matter. The existing mixed-filter stress pair rejects the new
single-cycle calibration contract; that does not imply arbitrary BRR timbres
are invalid or inaudible.

This milestone adds an owned periodic sample pair and measures its actual PCM.
Both use the existing normal tuning selector 0. Across base notes 24..36,
both slots and direct-GAIN/ADSR modes, all 52 measured captures are within
**5.857 cents** of an explicitly authored C4..C5 equal-tempered target, below
the fixed 10-cent limit. Base note 24 meaning C4 is a convention for these
owned fixtures, not a claim about vendor samples or the original bank.

The diagnostic program remains DA. No new pitch-table or runtime firmware
flag is required. Existing diagnostic assets, firmware image hashes and
production selection retain their previous behavior. General qualification
and playback remain false; private SGB program ROMs remain required.

## Sample and tuning contract

`build_sgb_owned_sample_fixture.py` constructs a 192-byte uploaded asset object:

- Physical slot 2, score ID 2: independently authored square, ADSR `$8F/$6F/$B8`.
- Physical slot 3, score ID 10: independently authored triangle, ADSR `$8E/$AF/$B8`.
- Each waveform has a fundamental period of exactly 32 decoded samples, encoded
  as two range-11, filter-0 BRR blocks. The final block has end+loop flags; each
  loop returns to its own start, with no predictor-dependent introduction.
- Both normal tuning selectors are 0. Sample starts remain in the preceding
  near relocation layout, with zero padding, existing bounded counts and IDs.

The asset SHA-256 is
`97a71e49a8b62073aae00e0ac1f5ef6830affcce3c28f1be8b59be6cf505c12b`.
The source asserts neither proprietary sample bytes nor proprietary timbre.
The normal thirteen-note pitch words produce approximately 260.742 Hz at the
low end and 522.461 Hz at the high end; corresponding authored targets are
261.626 Hz and 523.251 Hz. Quantized tuning remains within 10 cents. The measured
range is approximately -5.857..-1.108 cents across both waves and envelopes.
Selector 2 remains the preceding instrument-10 register table and is unsuitable
for this owned 32-sample C4 calibration; it is explicitly tested as a rejection.

The builder also emits a 32-KiB owned homebrew held-note cartridge, selecting
any note 24..36 and uploading the score plus the exact sample object through
one real 4096-byte SOU_TRN frame. Existing score admission, identity, release,
STOP and replacement semantics are unchanged. Export refuses overwrite.

## PCM measurement

`check_sgb_owned_sample_pitch.py` drives the existing DSP phase-clock runner
at the nominal 32000 Hz sample rate. Each isolated capture lasts 8192 stereo
frames (262144 DSP clocks), discards 2048 warmup frames and measures subsequent
rising zero crossings with linear interpolation. A minimum of twelve complete
periods, maximum inter-period deviation of one sample and a period in 32..256
output frames are required. Signed PCM length/type, stereo equality, adequate
positive/negative amplitude and absence of hard clipping are checked first.

The detector is intentionally limited to the independently authored waves,
each proved to have one positive crossing per fundamental cycle. It is not a
general pitch detector for stress BRR, arbitrary harmonics, noise or proprietary
samples. A 16-sample waveform at unchanged DSP pitch must fail the expected
C4 contract; a wrong selector must also fail. Frequency comes from PCM crossings,
not from a pitch register or inferred loop-length formula.

Captures use either constant direct gain `$60` or the actual uploaded ADSR
profile. Noise, pitch modulation and echo are disabled; one isolated slot plays
at a time. Both slots, thirteen notes and both envelope modes are mandatory.
Runner children have a 30-second timeout and must return exactly 32768 PCM bytes.
The JSON report retains frequency, error in cents, period statistics and hashes;
it does not retain PCM or private input. Its schema is
`gbb-sgb-owned-sample-pitch-v1`; qualification/playback remain false.

## Reproduce and validation

```sh
python3 scripts/build_sgb_owned_sample_fixture.py --note 24 \
  --output /tmp/owned-sample.gb
python3 scripts/check_sgb_owned_sample_pitch.py \
  --runner build-dmg-firmware/gameboy_snes_dsp_pcm_fixture_runner
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_owned_sample_pitch_'
```

The PCM matrix passes all 52 captures, with maximum absolute error
5.856463 cents. A repeated render checks exact PCM determinism. Public checks
cover independently decoded wave geometry and fundamental period, silent,
unipolar, clipped, irregular and malformed captures, wrong tuning, a shorter
waveform, stress-asset rejection, note bounds and refusal to overwrite exports.

Whole-host transport checks pass four physical runs: SGB1/SGB2 in native and
scalar modes, at note 24 with both sources active. All runs use the real uploaded
asset hash, two-block sample geometry, normal tuning and actual DSP pitch 1068
on both voices. Each run requires exact reset and restored whole-state/observer/
PCM continuation, positive audio and finite restore bounds. Native/scalar PCM
and envelope observations match within each model. The transport CTest passes
in 37.94 seconds. Six focused calibration/reference/fixture/build/DSP-envelope
CTests pass in 5.74 seconds, and bundled prototype reproducibility and
`git diff --check` pass. Earlier full physical firmware matrices were not rerun.

The frequency measurements are isolated DSP evidence. Whole-host tests above
prove upload, configured pitch and deterministic playback, but do not measure
whole-host audible frequency, combined-audio resampling, all notes or reversed
slot/voice mapping. They do not establish independent DSP arithmetic, physical
hardware accuracy, original sample timbre or title sound compatibility. No
private original execution or hardware check was performed for this milestone.
Performance optimization remains deferred.

## Next step

The [native whole-host pitch gate](sgb-host-sample-pitch.md) extends PCM
measurement across the full octave, both models, both voices and reversed
slot mapping. The [combined whole-host pitch gate](sgb-combined-sample-pitch.md)
adds 48-kHz rate conversion with silent and active GB audio. Combined source
transition continuity is the next acoustic gate before expanding the owned
instrument bank.
