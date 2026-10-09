# D8 asynchronous owned-sample envelope checks

The [D8 diagnostic](sgb-native-score-instrument-adsr.md) now has whole-host
owned-sample fixtures corresponding to the [asynchronous original reference](sgb-asynchronous-envelope-reference.md).
They check a held instrument-10 profile during peer retriggers, rests and E0
instrument changes, plus final-song clipping, in both voice orientations and
both physical slot arrangements. Exact reset/save-load replay includes complete
serialized state, envelope observations and owned PCM.

Envelope behavior is checked within explicit sample/latch phase bounds.
**Clipped song-end timing remains outside the retained reference allowance.**
The report exposes that mismatch; it does not mark timing qualified.
Qualification and production playback remain false, and SGB program ROMs remain
required. No firmware or production-path changes occur here. Performance
optimization remains deferred.

## Owned geometry and frozen images

`build_sgb_score_async_adsr_fixture.py` uploads a 2048-byte score and 192-byte
owned asset through the existing 4096-byte transfer frame. The three admitted
song roots share the authored pattern geometry. Both physical sample slots use
owned looping BRR, so natural sample completion cannot substitute for a KOF.

Instrument ID 10 maps to one owned slot with ADSR `$8E/$AF`, GAIN `$B8` and D7
tuning selector 2. Instrument ID 2 maps to the other with ADSR `$8F/$6F`, GAIN
`$B8` and selector 0. Reversing slots also reverses descriptor, tuning and score-ID
mapping placement. Both voices initially select instrument 10; the switch case
alternates only the peer between IDs 10 and 2. SRCN deliberately remains the
owned slot number, while pitch/envelope setup matches the corresponding
resident-instrument reference.

The compared first-pattern notes and controls match the original authored
streams. Duration-1 final rests are omitted because D8 does not admit them.
Full cases append a separate duration-16 rest pattern and finish at tick 80;
their preceding KOF/release observations remain checked. The clipping case
uses a single pattern and finishes at tick 32, matching the reference's early
song end. Adding a silent second pattern to that case would change the held
release and is explicitly avoided. Exporters refuse overwrite.

D8 retains SHA-256
`8cb99b32c080368a0573a7a0c125522e9d5940ef0a4cd59a51ab9971993c47c0`;
its 6585-byte SPC payload still leaves 71 bytes below `$1BFF`. D7 and the bundled
prototype are unchanged. No envelope, pitch, gate-table or scheduler extension
is introduced to make these checks pass.

## Read-only setup, envelope and replay checks

The existing dedicated ADSR probe still observes KON/KOF at SNES instruction
boundaries and published ENVX at physical 32-kHz sample boundaries. It now also
compares active notes' pitch, SRCN, ADSR, GAIN and cached tuning against their
onset setup. Every asymmetric scenario requires zero observed setup changes.
This detects changed values at observation boundaries; unlike the reference's
write trace, it does not audit same-value rewrites or changes entirely between
boundaries.

The preceding 30-element `envelope_notes` records retain their fields. Two
parallel arrays add `envelope_setup_changes` and `envelope_release_prefix`, the
first eight published ENVX values after each observed KOF. All new observer
state participates in exact uninterrupted/restored/reset equality. Existing
onset, peak, anchor, KOF and zero observations continue to add save/load
checkpoints. The diagnostic bounds remain 32 notes, 12000 samples per note,
140 million clocks, 4096 restores, 250000 PCM frames and 90 seconds per child.
The test output cap remains 16 KiB per probe result.

Every note requires exact descriptor/source/pitch/tuning, peak 127, zero missing
samples, monotonic decay/release, one-unit release drops and a real KOF followed
by zero. Earlier notes must reach zero before the same voice retriggers.
Peak offset may differ from the pinned reference by four output samples;
active ENVX anchors, last active ENVX and release-start ENVX by two units.
Fast instrument-2 sample-8 attack is separately excluded because one latch
phase can cross its full attack step; its peak and later anchors remain checked.
The held voice retains all slow-attack anchors, including sample 8. Clipping
compares its active prefix through sample 4096 rather than treating sample 8192
as active. Zero time remains `64*n-2 .. 64*n+130` SPC cycles after KOF for observed
release-start ENVX `n`.

Native SGB1 switch cases with held voice 3 expose an initial release prefix
`111,110,110,110,109,109,108,108` on the instrument-2 peer. This includes a
three-sample first inter-drop interval, consistent with a decay-to-release
latch transition.
The raw cadence-exception count remains one. Tests permit that single initial
interval only for instrument-2 switch notes, require one-unit steps throughout,
two-sample intervals thereafter and no other cadence exception in the entire
release. They validate the prefix against the preserved raw counter; they do
not reset or subtract the counter in the probe. The original fixture's exact
cadence is therefore distinguished from this native latch-transition case.

Native/scalar runs require identical owned PCM and envelope records. Combined
audio retains identical physical envelope/prefix/setup observations, with its
existing output-frame ratio. All modes independently require exact reset and
save/load replay within the same image; PCM is not compared to private samples.

## Timing mismatch is retained

The machine-readable test report is `gbb-sgb-score-async-adsr-v1`. It includes
raw per-note gate and onset times, original-reference bounds, and separate
`gate_matches`/`onset_matches` flags using the **unchanged 4096-SPC-cycle
allowance**. `timing_qualified` is false when any comparison misses. General
qualification/playback remain false independently of these diagnostic flags.

The corrected native clipping case releases at approximately 176700 SPC cycles,
while the original releases at 170746..170907. Its ENVX release trajectory and
exact replay pass, but its final KOF exceeds even the original bound plus 4096.
Envelope tests retain a separate 180000-cycle observation ceiling for this
case; that ceiling is not a reference-conformance tolerance. A public mutation
test specifically verifies that this timing miss remains visible.

Other timer-driven gates and all asynchronous onsets retain their preceding
reference-window comparison with the 4096-cycle allowance. The report preserves
each individual comparison and its raw timing.
Neither timestamps nor reference bounds are shifted to fit native execution.
This milestone establishes bounded envelope/replay coverage, not exact
asynchronous scheduling conformance.

## Reproduce and scope

```sh
python3 scripts/build_sgb_score_async_adsr_fixture.py --case switch \
  --held-voice 3 --slots 3,2 --output /tmp/native-asynchronous-envelope.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_adsr_probe
python3 tests/sgb_score_async_adsr_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe -v
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_score_async_adsr$'
```

The seven-method suite includes 32 asymmetric case/model/voice/slot scenarios
and six native/scalar/combined switch scenarios: 38 physical runs, each with
uninterrupted, restored and cold-reset execution. Public geometry, frozen-image,
export and timing-mutation checks require no private images. Expected envelope
anchors and original timing windows come from the preceding independently
executed register-only reference milestone; no new private execution is needed
to run this owned-sample suite.

All seven final test methods passed in two groups (165.329 and 241.433 seconds).
The 38 physical runs contain 150 per-note records and 62..114 exact in-place
restores per run, plus cold-reset replay. Re-evaluating all retained timing flags
confirms exactly eight misses, all clipped held KOFs at 176665..176710 SPC cycles;
all other gate and onset comparisons pass the retained reference-window
allowance. Four raw initial cadence exceptions remain visible, on two
instrument-2 notes in each SGB1 held-voice-3 slot arrangement.

Four preceding D8 build/trajectory/render test methods passed in 161.402 seconds,
including sixteen paired physical scenarios with the extended observer. Eleven
focused regression CTests passed in 105.66 seconds, including the owned
asynchronous reference fixture/parser suite, preceding envelope/chromatic/pitch
fixtures, native envelope/chromatic playback, DSP envelope, host, transfer and
firmware build/reproducibility contracts. The bundled prototype check retains
its preceding hash. Earlier private original executions and full D8 upload
lifecycle matrices were not rerun; firmware and original-reference observation
code are unchanged. `git diff --check` passed.

As before, the originals used this repository's DSP. Published-register
agreement does not independently validate DSP arithmetic against hardware or a
second emulator, nor reproduce original sample timbre or audible fundamental
frequency. No private instructions, sample/directory/table data, scores or PCM
are implementation inputs or committed artifacts. Active STOP, replacement and
recovery during these new asymmetric held shapes are not covered here; earlier
D8 lifecycle qualification remains separate.

## Next step

D8 remains frozen with its documented clipped timing miss. The
[D9 initial-tick diagnostic](sgb-native-score-initial-tick.md)
now implements that correction and directly compares fresh original clipped
gates within the retained 4096-cycle allowance. Full direct comparisons expose
a separate last-peer gate miss; the [DA peer-gate diagnostic](sgb-native-score-peer-gate.md)
adds a bounded correction and the complete four-case timing matrix. Broader
coverage and production integration remain separate. Performance optimization remains deferred.
