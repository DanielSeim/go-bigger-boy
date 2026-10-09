# Native whole-host sample pitch

The [owned sample calibration](sgb-owned-sample-pitch.md) now has a native
whole-host PCM gate. Actual uploaded DA firmware plays both authored instruments
through real JOYP/SOU_TRN transport. The checker measures audible frequency from
captured PCM across base notes 24..36, each voice and both sample maps on SGB1/SGB2.
Pitch registers are checked separately and never substituted for PCM evidence.

This remains a bounded diagnostic. General qualification and production playback
remain false, and private SGB program ROMs remain required. The owned C4..C5
convention and 10-cent tolerance are unchanged. Original bank timbre, physical
hardware and independent DSP arithmetic remain outside this milestone.

## Owned octave and mapping

`build_sgb_host_sample_pitch_fixture.py` builds a single-voice octave: seven
16-tick notes in the first pattern, six in the second, with matching rests on
the inactive voice. Both instrument IDs, 2 and 10, use the calibrated normal
selector 0. The score keeps the preceding three bounded song roots and 208
logical ticks; only one selection is requested.

A reversed map exchanges score IDs, descriptors and the two owned waveforms
between physical SRCNs 2/3. Start/loop addresses retain the physical near layout,
all sample counts remain two and padding stays zero. The active instrument can
therefore play on either voice and physical slot. Reversal never replaces the
sample's identity with a different waveform. Each fixture uses one 4096-byte
physical transfer frame; the exporter refuses overwrite.

The native matrix covers both models, both voices, both instruments and both
maps: 16 runs and 208 notes. Two additional scalar runs cover voice 3,
instrument 10 and the reversed map on both models. Their complete PCM windows,
whole-run PCM and envelope observations must equal the matching native run.

## Native PCM capture and replay

The separate `gameboy_sgb_host_sample_pitch_probe` target enables
`GBB_SCORE_ACOUSTIC_PROBE` plus the existing ADSR observer. Ordinary probes keep
their preceding output and behavior. No emulator-core, DSP arithmetic or runtime
firmware changes are needed; DA retains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

At each instruction-boundary KON observation the probe records the actual native
sample counter, voice, SRCN and pitch. It captures the next 2048 consecutive
native output frames. Both output channels must agree for the centered isolated
voice. At most thirteen windows are admitted. Windows must finish before the
observed key-off, without changed setup or missed published samples. No sample
is synthesized, shifted or corrected by the observer.

The checker discards 128 warmup frames and applies the same PCM crossing detector
as the isolated calibration. At least twelve complete periods, inter-period
variation no larger than one sample, sufficient bipolar amplitude and no hard
clipping remain mandatory. The 32000-Hz rate comes from the running host and
must match the native clock contract. The detector remains specific to the
known single-cycle owned waves, not arbitrary BRR timbres.

Each execution repeats from a fresh host with exact in-place save/load and then
from cold reset. Complete host state, whole-run PCM, envelope observer state
and all captured windows must match. Additional snapshots occur every 512 captured
frames, alongside the preceding command/envelope checkpoints, so replay includes
partially collected windows. Capture buffers belong to the diagnostic caller;
they are compared across executions and are not additions to the emulator's
state format.

Every child retains 70 million master clocks, 90 seconds, at most 4096 restores
and a 256-KiB JSON cap. Each window is bounded to 2048 signed int16 samples;
exactly thirteen windows/notes are required. Missing/reordered/duplicate notes,
wrong map/setup/identity, malformed PCM or a pitch error beyond 10 cents reject.
The dedicated probe rejects combined output because its output sample counter
has different rate-conversion semantics. The complete matrix CTest has a
2400-second timeout.

The resulting `gbb-sgb-host-sample-pitch-v1` report retains bounded frequency,
period, error, replay metadata and hashes. Raw owned PCM is temporary child
output and is omitted from the final report. No private images, instructions,
samples, scores or PCM are implementation inputs or committed artifacts.

## Reproduce

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_host_sample_pitch_probe
python3 scripts/check_sgb_host_sample_pitch.py \
  --probe build-dmg-firmware/gameboy_sgb_host_sample_pitch_probe
python3 scripts/build_sgb_host_sample_pitch_fixture.py \
  --voice 3 --instrument 10 --reversed-map --output /tmp/host-pitch.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_host_sample_pitch'
```

Public guards use synthetic PCM, with an explicit octave error at unchanged
register pitch, malformed windows and wrong gate/setup metadata. Their reports
are parser tests, not additional original or whole-host measurements.

## Validation evidence

All 16 native matrix runs and two scalar controls pass: 234 measured windows,
including 208 native note measurements. Maximum absolute pitch error is
5.852163 cents (SGB1 maximum 5.851278, SGB2 maximum 5.852163), below the unchanged
10-cent bound. Every window contains at least fifteen measured complete periods;
maximum period deviation is 0.048806 samples. The scalar controls match their
native counterparts' complete windows, PCM and envelope observations exactly.

Every run passes cold-reset and complete restored state/PCM/observer replay,
with 284..287 exact in-place save/load checkpoints. The initial reversed-map
instrument-10 SGB2 smoke capture also passed. No detector or timing allowances
were expanded after observing these results.

Five public fixture/export/parser/mutation methods pass. Seven focused CTests
pass in 43.09 seconds, including the new public guards, preceding isolated
calibration, four preceding real-upload/native-scalar replay runs, direct-timing
comparison guards, firmware build/reproducibility and DSP envelopes. CMake's
full-matrix registration, bundled prototype reproducibility and `git diff --check`
pass. The full matrix was executed through the same checker command directly;
it was not redundantly rerun through CTest. Earlier broad physical firmware
matrices and private-original/hardware/independent-DSP checks were not rerun.

## Next step

Qualify audible pitch after combined-audio rate conversion, starting at the
48-kHz frontend rate with the GB source silent. Keep the 10-cent limit, capture
real output timestamps and exact replay, then test mixing with active GB audio.
Broader sample banks and real-title acoustic compatibility remain separate work;
performance optimization remains deferred.
