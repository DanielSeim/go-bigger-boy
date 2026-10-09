# Combined whole-host sample pitch

The [native whole-host pitch gate](sgb-host-sample-pitch.md) now extends to real
48-kHz combined output, with the GB source explicitly silent and with an owned
GB pulse active. Both source frequencies are measured from actual output PCM.
This is a bounded diagnostic: qualification and production playback remain
false, and proprietary SGB1/SGB2 program ROMs remain required for production.
Physical hardware, independent DSP arithmetic, original-bank timbre and real-title
acoustic compatibility are outside this evidence.

## Owned inputs and coverage

`build_sgb_combined_sample_pitch_fixture.py` preserves the preceding owned
single-voice octave, score, upload, JOYP commands and reversible sample map.
An independently authored entry trampoline at `$3F00` either disables the GB
APU or initializes a constant channel-1 pulse. It returns to the preceding
`$0150` entry. The builder checks for unused trampoline space and the expected
entry, recalculates the cartridge checksum and refuses export overwrite.

The active fixture uses 50% duty, envelope volume 4, no sweep, no length gate,
frequency register `$700` and NR51 `$10` (channel 1 left only). The SNES voice
remains centered. Consequently the right channel measures resampled SNES pitch;
left minus right measures the GB pulse in the actual sum. This checks active
mixing without asking a single-wave detector to separate two simultaneous
frequencies in one channel. It does not qualify arbitrary stereo routing.

The expected pulse frequency comes from the modeled oscillator, not its PCM:
SGB1 is `21477273 / 5 / 8192` Hz and SGB2 is `4194304 / 8192` Hz. Only the known
constant pulse's channel difference is centered at its level midpoint to remove
DAC DC before crossing measurement. SNES PCM is never centered or corrected.
Mixing retains the host's provisional default gains of 16384/32768 per source.
These gains are not a hardware calibration.

The matrix covers both models, voices 2/3, instrument IDs 2/10, both physical
sample mappings and both GB-source states: 32 runs and 416 SNES note windows.
Four additional scalar controls cover voice 3, instrument 10, reversed mapping
on each model with GB audio silent and active. Whole-run PCM, complete stereo
windows and envelope observations must match the corresponding batched run.
All 36 runs include cold reset and complete save/load replay.

## Actual output capture

The separate `gameboy_sgb_combined_sample_pitch_probe` target opts into combined
capture. At instruction-boundary KON observations it records the drained output
frame counter, not a converted native DSP counter. It collects the next 3072
consecutive stereo output frames, recording actual APU half-clock boundaries.
Each window must finish before the observed key-off with unchanged voice setup
and no missed native sample publication. The 64-ms duration matches the native
2048-frame window; the initial 192 frames are excluded from pitch measurement.

The detector retains the 10-cent limit, at least twelve complete periods,
maximum one-output-sample period deviation, sufficient bipolar amplitude and
no hard clipping. Period bounds scale with the known 48000-Hz rate; native
32000-Hz defaults are preserved. No waveform, output timestamp or pitch register
is adjusted to fit a target. The estimator remains limited to the independently
authored single-cycle waves and the known GB pulse.

The probe also requires captured GB input samples and zero mixer clipping for
the whole execution. Silent GB windows must have exactly equal output channels;
active GB windows must contain the expected pulse in their channel difference.
Output frame counters, stereo buffers, half-clock bounds, source counters and
complete host state participate in reset/save-load equality. Snapshots include
partly collected windows at 512-frame checkpoints. These caller-owned buffers
are not additions to the emulator save-state format.

Every child retains 70 million master clocks, a 90-second deadline, a 4096-restore
bound and a 768-KiB JSON cap. At most thirteen 3072-frame stereo windows are
captured. Reports retain frequencies, errors, periods, source/replay metadata and
hashes; raw PCM is temporary probe output. No private images, instructions,
samples, scores or PCM are implementation inputs or committed artifacts. No new
private-reference execution is part of this milestone. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.
No production firmware, mixer, DSP or emulator-core behavior changes are needed.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_combined_sample_pitch_probe
python3 scripts/check_sgb_combined_sample_pitch.py \
  --probe build-dmg-firmware/gameboy_sgb_combined_sample_pitch_probe
python3 scripts/build_sgb_combined_sample_pitch_fixture.py \
  --voice 3 --instrument 10 --reversed-map --active-gb --output /tmp/combined-pitch.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_combined_sample_pitch'
```

The complete matrix CTest has a 3600-second timeout. Public guards exercise
owned fixture geometry, checksum/routing, silent and active synthetic PCM on
both model clocks, wrong source pitch despite unchanged registers, missing GB
contribution, right-channel contamination, clipping, gate crossings and malformed
windows. Synthetic PCM proves parser behavior; it is not whole-host evidence.

## Validation evidence

All 32 combined matrix entries and four scalar controls pass: 468 SNES pitch
windows, including 416 matrix note measurements, and 234 active GB-pulse
measurements. Maximum absolute SNES error is 5.887801 cents on each model;
maximum GB-pulse error is 0.093298 cents. Both stay below the unchanged 10-cent
limit. SNES windows contain at least fifteen measured periods with maximum
period deviation 0.255669 output samples; GB windows contain at least twenty-nine
periods with maximum deviation 0.163831 samples.

Every run passes complete cold-reset and restored state/PCM/observer equality,
with 336..339 in-place save/load checkpoints. All four scalar controls exactly
match their batched counterparts' stereo windows, whole-run PCM and envelope
observations. Each execution captures 148586 GB source samples; every whole-run
mixer clipping count is zero. The complete matrix was executed directly through
the registered checker, without a redundant full CTest rerun.

Five new public fixture/export/parser/mutation methods pass. Seven focused
CTests pass, covering combined and native public guards, isolated owned DSP
pitch, preceding real-upload/native-scalar replay, DSP envelopes, and firmware
build/reproducibility. The first five completed in 43.21 seconds; firmware
build/reproducibility completed in 0.34 seconds. The final five-method combined
contract was rerun through CTest after its additional guards were added and
passed in 1.35 seconds. A separate SGB2 native/scalar instrument-10 reversed-map
capture still matches exactly, with 287 restores and maximum error 5.828688
cents. All three affected probe targets build; CMake registration, new-file
whitespace checks and `git diff --check` pass.

No new private-original, hardware or independent-DSP comparison was performed.
Earlier broad firmware/title matrices were not rerun for this diagnostic-only
change. The evidence covers the default modeled clocks and 48-kHz output;
other frontend rates, arbitrary source waveforms and transition behavior are
not qualified by these steady-state captures.

## Next step

The [combined source-transition gate](sgb-audio-transitions.md) now checks SOUND
stop/restart and GB routing mute/unmute with matched owned controls and complete
stereo replay. Next qualify overlapping owned SNES voices with active GB audio,
including independent release and retrigger. Broader sample banks and real-title
acoustic compatibility remain separate work; performance optimization remains
deferred.
