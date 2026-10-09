# Bounded uploaded instrument-10 ADSR profile

The opt-in D8 diagnostic extends [D7 exact-octave tuning](sgb-native-score-instrument-tuning.md)
with exactly one additional owned-slot envelope descriptor: ADSR1 `$8E`, ADSR2
`$AF`, GAIN `$B8`. It uses the [measured envelope reference](sgb-instrument-envelope-reference.md)
to check actual whole-host ENVX trajectories. Qualification and production
playback remain false; SGB1/SGB2 program ROMs remain required. Performance
optimization remains deferred.

## Build and admission

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --upload-recovery --instrument-mapping \
  --instrument-tuning --instrument-envelope --output /tmp/adsr-host.rom
python3 scripts/build_sgb_score_adsr_fixture.py --score-profile held \
  --envelope-profile both --tuning 2,2 --output /tmp/adsr-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_adsr_probe
python3 tests/sgb_score_adsr_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe
```

The flag is strictly boolean and requires D7 and its prerequisites. The 256-KiB
image SHA-256 is
`8cb99b32c080368a0573a7a0c125522e9d5940ef0a4cd59a51ab9971993c47c0`.
The SPC payload is 6585 bytes (`$0200..1BB8`), leaving 71 bytes inside the unchanged
`$0200..1BFF` cap. The host remains 1529 bytes (`$8000..85F8`). D7 retains its
preceding exact hash; earlier images and the bundled prototype are unchanged.
Exporters refuse overwrite.

The asset and score layouts are unchanged. Both physical slots are fully
validated before directory readiness or audio. Existing ADSR/direct-GAIN
profiles retain their meaning. The new path requires the complete `$8E/$AF/$B8`
combination; `$8E/$6F`, `$8F/$AF`, ADSR-off with `$AF`, unsupported GAIN and
neighboring reserved data reject. No arbitrary envelope fields or resident
instrument-table data are admitted.

Score IDs still resolve through the D6 map to owned SRCNs 2/3. Envelope and
D7 tuning selectors remain independent. E0 applies the selected slot's profile
on that voice; absent E0 inherits it. Fresh playback defaults to slot 2.
Repeated prefixes, executed ends and clipped tails retain their preceding
semantics. Atomic upload clearing and recoverable rejection remain in force,
with D8 identification in host, bridge and recovery paths.

## Two measured long-note gates

The preceding gate table admitted at most duration 24. The measured held and
early-release fixtures use duration 64, so D8 adds only these two entries:

| Tempo | Articulation | Duration | Timer pulses |
| --- | --- | --- | --- |
| 96 | 127 | 64 | 164 |
| 96 | 63 | 64 | 100 |

The finite lookup grows from ten to twelve entries, using eight previously
unused table bytes before `$1000`. The original ten entries and all previous
images retain their bytes. These counts target the observed original KOF
windows (roughly 335000 and 204000 SPC cycles), with actual whole-host key-edge
observations checked within the existing 4096-cycle allowance. No timing
interpolation or general duration law is introduced. Duration 64 at tempo 128
still rejects silently.

The native fixtures use a separate duration-16 rest pattern after each measured
shape. The original reference uses a duration-1 final rest, which the native
bounded grammar does not admit. This affects the silent completion timeline,
not the compared preceding note, gate or envelope. Held/short native cases
finish at tick 80; two-note retriggers finish at tick 48. Their first-pattern
notes match the original reference shapes exactly.

## Read-only whole-host observations

A dedicated compile-time build of the existing transport probe observes rising
physical KON and KOF register bits at SNES instruction boundaries. It samples
published ENVX when the physical 32-kHz DSP output count advances, including
in combined-audio mode. Missing samples, nonmonotonic decay/release, release
steps larger than one and incorrect two-sample release cadence are counted and
must remain zero in the trajectory tests. It observes the first generation's
song 1 only, up to 32 note records and 12000 samples per note.

Onset, peak, KOF, zero and anchor observations add save/load checkpoints. Every
scenario compares complete serialized state, all envelope-observer state and
owned PCM exactly across uninterrupted, restored and cold-reset runs. These are
within-image replay checks. Different image sizes shift upload/startup clocks,
so whole-run PCM fingerprints are not compared between D7 and D8.

The optional `envelope_notes` output consists of compact arrays with these
fields, followed by eleven ENVX points at offsets
`1,4,8,16,32,64,128,512,2048,4096,8192` (255 means unobserved):

`voice, SRCN, ADSR1, ADSR2, GAIN, tuning, pitch, peak, peak_sample_offset,
active_last, release_start, release_drops, bad_cadence, bad_decay, bad_release,
missed_samples, KON_half, KOF_half, zero_half`.

The dedicated test's JSON cap is 16 KiB for these bounded per-note records.
Other probe variants keep their preceding fields/behavior. Clock, snapshot,
PCM-frame and child-time bounds remain 140 million, 4096, 250000 and 90 seconds.
No emulator-core or production-path changes are needed.

## Envelope comparison and limits

Trajectory fixtures use owned looping BRR in both slots, preventing natural
sample completion from substituting for a KOF release. The new profile is tested
on each slot/voice with mixed preceding/new envelopes, reversed score maps and
independent normal/half/instrument-10 tuning. Descriptor and pitch observations
must match exactly.

The comparison preserves raw observations and allows finite global-rate/KON
latch phase differences: peak offset within four output samples of the original
reference, held/decay ENVX anchors and final active ENVX within two units. For
the preceding fast-attack instrument-2 control, sample-8 ENVX is excluded from
anchor comparison because a phase shift can cross its full attack step; later
anchors and peak remain checked. The slow new attack retains sample-8 checking.
Every trajectory must visit 127, decay monotonically, have an explicitly observed
KOF and release to zero. Release decrements must be one ENVX unit every two DSP
output samples. Zero time is bounded by the observed release-start value and
128 SPC cycles of sample/latch phase, plus two cycles of instruction-boundary
observation allowance. No reported timestamp is adjusted.

The first held-note run on both new slots reaches 127 at offset 199, two samples
later than the reference's offset 197. It matches the later held anchors
125/113/99/79 at offsets 512/2048/4096/8192 and releases from 70 to zero.

Private originals were measured in the preceding reference milestone with this
repository's DSP. Matching those register-driven trajectories does not
independently validate DSP arithmetic against hardware or another emulator.
No original waveform/sample, PCM, instructions, instrument table or private
score is used as an implementation input. Acoustic and title qualification
remain outstanding. Adding these pitch/envelope settings does not reproduce
instrument 10's sample, timbre or audible fundamental frequency.

## Validation evidence

All nine ADSR test methods passed, covering 66 physical scenarios across SGB1
and SGB2. The three trajectory methods passed in 138.480 seconds; the five
lifecycle methods passed in 338.632 seconds. The remaining build/cap method
also passed in the thirteen-method exporter, frozen-image and reproducibility
suite (5.701 seconds). The physical cases include native/scalar/combined audio,
slot and tuning independence, inherited selection, clipped endings, replacement,
STOP, malformed-input rejection, fragmented recovery and D7 compatibility.
Each physical scenario includes exact reset/save/load state and owned-PCM replay.

Ten focused regression CTests passed in 100.10 seconds: original build and
reproducibility, original transfer and host contracts, envelope/chromatic/pitch
fixtures, native envelope/chromatic behavior and the DSP envelope contract.
The bundled prototype hash remains
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`;
its reproducibility check and `git diff --check` passed. Earlier full upload
matrices were not rerun; the build suite checks their frozen image hashes.
No new private-original execution was needed for this milestone; the trajectory
expectations come from the preceding documented reference runs.

## Next step

The [asynchronous original reference](sgb-asynchronous-envelope-reference.md)
now measures a held voice while its peer retriggers, rests or changes instrument,
including reversed voices and clipped endings. The new
[D8 whole-host checks](sgb-native-asynchronous-adsr.md) cover owned held/peer
envelopes and exact reset/save-load/PCM replay, while retaining an explicit
clipped song-end timing mismatch. Next, correct that scheduling offset in a
bounded successor before broader instrument/sample compatibility or production
integration.
