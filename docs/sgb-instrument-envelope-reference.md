# Register-only instrument-10 envelope reference

Newly authored held-note, early-release and retrigger fixtures now measure
published ENVX on voices 2 and 3 while both private original models execute.
Instrument 2 is a fresh control. This defines the next bounded envelope
extension after [D7 exact-octave tuning](sgb-native-score-instrument-tuning.md);
it does not broaden the native envelope allowlist yet. Qualification and
production playback remain false; SGB1/SGB2 program ROMs remain required.

## Observation and provenance

`build_sgb_instrument_envelope_fixture.py` sends one owned pattern through
actual JOYP/SOU_TRN transport. Both channels use E0 instrument 2 or 10, pan 10,
track volume 127, song volume 160 and tempo 96. Each stream ends with an explicit
duration-1 rest and terminator. The cases are:

| Case | Notes | Duration | Articulation | Instrument-10 owned cartridge SHA-256 |
| --- | --- | --- | --- | --- |
| held | 24 | 64 | 127 | `7838e5982fb2f7fe1089b2994ca6c7f60ced80614de66ef5ccdf8831e7c8b11b` |
| short | 24 | 64 | 63 | `973758a4dbc0f8bf0753579b9612480052581bc37d1533cc1d9f0bac38af92d9` |
| retrigger | 24, 25 | 16 each | 127 | `a79a6b692bf16be78bd6727f20cdbde3f8230486b1ff301ca6b726745c129319` |

The trace tool's new `--voice-envelope-trace` option requires fractional APU
synchronization and a sound-event output file. It reads published ENVX registers
`$28/$38` once per DSP output sample, at the existing phase-27 output boundary.
These `E` rows contain register address/value and sample/SPC-cycle timestamps.
They are not full internal envelope values, waveform samples or PCM.

Sampling is anchored to the first combined KON and bounded to exactly 12000
samples per voice (0.375 seconds at 32 kHz). DSP write observation also stops at
that window in this mode. Legacy private RAM (`R`) and internal renderer-state
(`V`) diagnostic exports are disabled for this mode. Defaults retain their
preceding behavior. No emulator-core, scheduler, DSP arithmetic or firmware
bytes change in this measurement milestone.

The checker requires exact source, pitch and envelope setup at every onset:
instrument 2 uses SRCN 2, ADSR `$8F/$6F`, GAIN `$B8`; instrument 10 uses SRCN 10,
ADSR `$8E/$AF`, GAIN `$B8`. Notes 24/25 retain their preceding pitch words.
Every note requires an actual observed KOF; releases cannot be inferred from
a retrigger. Both voices must have the complete ordered sample window, the
same sample clocks and no missing/duplicate observations. Only setup, bounded
ENVX summaries and input hashes leave temporary storage.

Private firmware is executed with this repository's DSP implementation. These
checks independently observe original firmware setup/control behavior, but do
not independently validate the DSP envelope arithmetic against hardware or a
second emulator. Original instrument tables, instructions, samples, directory
contents, private scores and PCM are not implementation inputs or committed
artifacts. Raw private traces and child stdout/stderr remain temporary.

## Measured behavior

Both voices and models agree exactly on the initial held-note ENVX anchors:

| Output samples after KON | Instrument 2 ENVX | Instrument 10 ENVX |
| --- | --- | --- |
| 1 | 0 | 0 |
| 4 | 0 | 0 |
| 8 | 64 | 2 |
| 16 | 127 | 6 |
| 32 | 127 | 18 |
| 64 | 127 | 38 |
| 128 | 126 | 82 |
| 512 | 123 | 125 |
| 2048 | 111 | 113 |
| 4096 | 97 | 99 |
| 8192 | 74 | 79 |

Instrument 2 first reaches 127 at sample offset 9; instrument 10 at offset 197.
On the second retriggered note these offsets are 10 and 198..199 respectively.
The retriggered instrument-10 anchors at offsets 32/128 are 16/80, reflecting
the different DSP/global-rate phase. Both instruments visit zero again before
attack and decay monotonically after the peak. These are fixture-specific
published-register observations, not universal attack times for every phase.

| Case | Instrument 2 ENVX before release | Instrument 10 ENVX before release | Observed gate SPC cycles, both voices/models |
| --- | --- | --- | --- |
| held | 63 | 70 | 334490..335212 |
| short | 84 | 87 | 203415..204126 |
| retrigger, note 24 | 110 | 111 | 72334..73044 |
| retrigger, note 25 | 110 | 111 | 72691..73400 |

All observed releases are monotonic and reach zero, including before the next
retrigger. Each published ENVX decrement is exactly one unit, two output samples
apart (64 SPC cycles). For observed release-start ENVX `n`, zero occurs between
`64*n` and `64*n+128` SPC cycles after KOF, covering the sample/latch phase.
Settled ENVX remains zero until another KON. Both-model gates agree within
1024 SPC cycles, peak offsets within one sample and release-zero timing within
128 SPC cycles. Exact anchors and release metadata are pinned; gate bounds
remain held 334000..336000, short 203000..205000 and retrigger 72000..74000.
No tolerance is substituted for source/descriptor or ENVX anchor equality.

## Reproduce and validate

```sh
python3 scripts/build_sgb_instrument_envelope_fixture.py --instrument 10 \
  --case held --output /tmp/instrument-10-envelope.gb
cmake --build build-dmg-firmware --target gameboy_snes_65c816_apu_trace
python3 scripts/check_sgb_instrument_envelope_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'sgb_instrument_envelope_(fixture|local_reference)$'
```

The checker covers twelve instrument/case/model runs, 32 per-note/voice
trajectories and 288000 ENVX register observations. Four additional held-note
runs compare sampler-off/on DSP write fingerprints within the same observation
window on both models. They agree exactly: 2500 writes on SGB1 and 2209 on SGB2.
This witnesses unchanged observed DSP write execution when sampling is enabled;
it is not an acoustic or hardware comparison.

The existing 8000000-instruction bound, 180-second child timeout, 16-MiB trace
cap and fewer than 32768 rows remain in force. Only instruction-bound completion
is accepted. No partial report is emitted on failure. The schema is
`gbb-sgb-instrument-envelope-reference-v1`, qualification/playback false. The
private CMake test is enabled only when all caller-owned firmware inputs exist;
public fixture/parser/CLI tests need no private images.

The final end-to-end private CTest passes all sixteen child runs in 117.21
seconds. Ten selected public/contract CTests pass in 102.76 seconds, including
native envelope/chromatic playback, the DSP envelope contract, fixtures, host,
transfer and firmware build/reproducibility. After final witness/hash guards,
the seven public envelope tests pass again (1.57 seconds through CTest). Five
preceding phrase fixture/helper tests also pass. The bundled prototype hash
check and `git diff --check` pass. Earlier long D7 physical matrices were not
rerun because the D7 image and probe are unchanged.

Reset/save-load envelope qualification for the next native profile is still
outstanding. Physical-device, independent-DSP, acoustic and title checks were
not performed. The D7 and bundled images are unchanged.

## Next bounded envelope contract

Implement an opt-in D8 successor requiring D7 and preserving all earlier image
hashes, production defaults, fixed sample regions and the `$0200..1BFF` code cap.
Admit exactly one additional uploaded descriptor combination per physical slot:
ADSR1 `$8E`, ADSR2 `$AF`, GAIN `$B8`. Keep all preceding ADSR/direct-GAIN profiles.
Reject new cross-combinations such as `$8E/$6F`, `$8F/$AF`, unsupported GAIN,
partial descriptors or nonzero reserved fields. Do not admit arbitrary envelopes
or copy a resident descriptor table.

Envelope choice remains attached to owned slots 2/3, independently of tuning
selector and score ID. E0 resolves through the D6 map, applies that slot's
profile and preserves per-voice inheritance. Fresh score playback defaults to
slot 2. Ordered prefixes, executed ends and clipped tails retain their existing
semantics. Validate both descriptors before directory readiness, and preserve
atomic upload invalidation and recoverable rejection.

Use owned looping BRR on both slots for trajectory tests so natural sample
completion cannot replace the gate's KOF release. Check the held, early-release
and retrigger shapes above on each voice/slot, mixed preceding/new envelopes,
independent tuning selection, actual DSP setup and explicit key-offs. Observe
peak, gradual attack, monotonic decay/release, release cadence and zero tails.
Account for the native program's different global-rate/latch phase explicitly;
do not infer exact original absolute timestamps or change observed timestamps
to force a match. Establish and document finite phase bounds before claiming
coupled ENVX agreement. Retain exact register setup and release-step checks.

Also check defaults, inheritance, maximum prefixes, end controls, clipping,
active replacement, STOP/reupload, malformed descriptors and fragmented cold/
active retries on both models. Reset and save/load must preserve full state,
per-note envelope observations and owned PCM exactly within the same image.
Adding the descriptor does not reproduce the original sample or timbre;
sample/acoustic equivalence stays separate. Performance optimization remains
deferred.
