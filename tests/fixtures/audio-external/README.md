# External audio references

This directory is reserved for provenance-tracked captures from real DMG/CGB
hardware or a specifically documented trusted emulator. It is intentionally
separate from [`../audio`](../audio), whose files are generated deterministic
GBB baselines.

Do not copy a generated capture here. Capture the same pulse, wave, and noise
fixtures at 48 kHz, 16-bit PCM, stereo, then convert each WAV with:

```sh
python3 scripts/audio_reference.py convert capture.wav dmg-pulse.txt \
  --name pulse --model dmg --source hardware \
  --provenance "DMG-01, line-out, take-1"
```

## SameBoy digital reference

SameBoy is a useful trusted *digital* reference because it models individual
DMG, MGB, SGB, and CGB revisions and exposes sample-accurate APU output. It is
not a substitute for an analog hardware capture: this path does not validate
DAC gain, the console's high-pass response, amplifier noise, or LCD/audio
coupling. Build and record with a pinned SameBoy revision using its audio
callback/recording API, then convert the resulting stereo WAV explicitly:

This repository includes a minimal fixture producer for that purpose. After
building SameBoy's `lib` target, compile it from the repository root with:

```sh
cc -std=c11 -I/path/to/SameBoy -I/path/to/SameBoy/Core \
  scripts/sameboy_audio_capture.c \
  -L/path/to/SameBoy/build/lib -lsameboy -lm -ldl \
  -Wl,-rpath,/path/to/SameBoy/build/lib -o /tmp/sameboy-audio-capture
/tmp/sameboy-audio-capture dmg pulse /tmp/sameboy-dmg-pulse-96k.wav
```

Run it for `dmg`/`cgb` and `pulse`/`wave`/`noise`, taking three independent
captures per fixture. The helper uses the same register sequences as the GBB
waveform contract and writes 96 kHz WAV output.

```sh
python3 scripts/audio_reference.py convert sameboy-dmg-pulse-96k.wav \
  sameboy-dmg-pulse-take1.txt --name pulse --model dmg \
  --source trusted-emulator --comparison normalized --downsample 2 \
  --provenance "SameBoy <commit>, GB model DMG, 96 kHz callback"
```

The reproducibility pin for this workflow is recorded in
[`sameboy-reference-pin.json`](sameboy-reference-pin.json): SameBoy v1.0.3 at
commit `208ba4afabffab9edde416f2dbb8ae459e34adb8`. Always build that exact
commit (rather than a moving branch) when producing reviewed references.

The `--downsample 2` flag is required for SameBoy's 96 kHz output. Conversion
uses a deterministic two-frame box average (discarding only an incomplete
trailing source frame from a run-loop overshoot) and then the same 64-level
quantization as the contract test; no rate conversion is performed implicitly.
SameBoy references should use `--comparison normalized`: this compares the
shape of each channel after removing its DC mean and normalizing its RMS, so
the gate is insensitive to known emulator mixer gain/filter differences while
still detecting timing, duty-cycle, wavetable, and LFSR changes. Raw comparison
remains the default for hardware captures and software baselines.
Keep the exact SameBoy commit, model/revision, callback rate, fixture ROM,
window alignment, gain, and take number in `provenance`. Do not mix hardware
and trusted-emulator captures in one aggregate.

Use `--start-frame` to select the fixture window after measuring the capture's
latency. Convert at least three takes, then aggregate them to establish a
repeatability-based tolerance:

```sh
python3 scripts/audio_reference.py aggregate \
  dmg-pulse-take1.txt dmg-pulse-take2.txt dmg-pulse-take3.txt dmg-pulse.txt \
  --source trusted-emulator --comparison normalized --margin 3 \
  --provenance "SameBoy <commit>, DMG, three takes, startup trim 16 frames"
```

The aggregate command uses a per-sample median and writes measured
`max_abs_error`/`rms_error` values plus a one-quantum safety margin by default.
For the pinned SameBoy fixtures, review against the GBB baseline and retain a
small explicit margin (the current six-fixture review uses three normalized
units); do not inflate tolerances to mask a shape mismatch.
Record the console/emulator revision, capture path, gain, alignment offset,
and number of takes in the provenance field or an accompanying review note.
Only reviewed external files (`*.txt`) should be committed here; the release
workflow automatically runs the external gate when they are present. The
strict gate accepts `source=hardware` and `source=trusted-emulator` (and keeps
the unpublished `source=external` spelling for compatibility).
`comparison=normalized` files must be sourced from `trusted-emulator`; omitted
comparison metadata is treated as the legacy `raw` mode.
