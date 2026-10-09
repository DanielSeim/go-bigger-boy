# Controlled vendor pitch and instrument observations

The [directory fixture](sgb-song-selection-fixtures.md) validates selection of
music IDs 1..3. This next fixture gives each ID a single independently authored
note on logical channel 2. It observes accepted DSP writes from the caller-owned
original program, without inspecting its instructions or extracting samples.

```sh
python3 scripts/build_sgb_pitch_fixture.py --instrument 2 --output /tmp/pitch-2.gb
python3 scripts/build_sgb_pitch_fixture.py --instrument 10 --output /tmp/pitch-10.gb
python3 scripts/check_sgb_pitch_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The shared cartridge builder retains the preceding fixture's header, checksums,
VRAM transfer and actual JOYP packet generation. The new 128-byte bank at `$2B00`
has three phrase roots, each containing one pattern with only channel 2 populated.
The tracks set E0 instrument 2 or 10, E1 pan 10, E5 song volume 160, ED track
volume 127 and E7 tempo 96, followed by duration 16, articulation `$7F`, one note,
a duration-1 rest and an end. The three base notes are 24, 25 and 36 (encoded
`$98`, `$99`, `$A4`). SOUND requests IDs 1, 2 and 3 sixteen frames apart. The
transport then ends with the conventional zero-length jump to `$0400`.
Instrument IDs refer to the reference's resident instruments; the fixture
contains no instrument table, BRR sample or copied score/program data.

## Measured contract, 2026-10-06

Both private original program images produced identical DSP setup for these
fixtures with GBB's independent SGB1/SGB2 GB bootstraps and the private original
SPC IPL. All three nonzero KON writes selected physical DSP voice 2 (mask `$04`)
with that voice's NON bit clear. These are DSP register values, not measured
waveform frequencies or a PCM equivalence result.

| Instrument | Base note 24 pitch | Base note 25 pitch | Base note 36 pitch | SRCN | ADSR1 | ADSR2 | GAIN |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | 1068 | 1132 | 2140 | 2 | `$8F` | `$6F` | `$B8` |
| 10 | 7993 | 8472 | 16016 | 10 | `$8E` | `$AF` | `$B8` |

SRCN, ADSR1, ADSR2 and GAIN were stable across the three notes for each instrument.
GAIN here records the register write; it does not establish the active envelope
level when ADSR is enabled. The semitone ratios are approximately 1.059925 and
1.059927; the octave ratios are approximately 2.003745 and 2.003753. Quantized
pitch words do not double exactly. Changing the instrument changes absolute
pitch as well as source/envelope setup, so a universal note-only pitch table is
insufficient for these cases. This does not identify the underlying tuning
algorithm or the samples' fundamental frequencies.

| Independently authored fixture | SHA-256 |
| --- | --- |
| Instrument 2 | `b189855a06911879d605c71dc94950fa264a6c6399a15a2a6ab9a2b0894f4f94` |
| Instrument 10 | `9493e26e50daece6a6ffe78567a60b281833e684637dc0f241630ebd8bb10c8b` |

The checker prints those hashes and the program/IPL/bootstrap hashes with only
selected note-setup metadata. Reference image pins match the preceding
[selection observation](sgb-song-selection-observation.md). Each of the four
runs is bounded to 8000000 SNES instructions and a 180-second child timeout.
Only the expected instruction-bound exit is accepted. Child stdout/stderr and
raw trace files remain temporary and are never forwarded. The trace helper's
CSV can contain private RAM diagnostics; the checker discards them and retains
only voice-2 pitch/source/envelope setup at KON. No PCM or sample data is exported.

The gate rejects a trace at the 32768-row cap, files above 16 MiB, malformed or
unordered DSP events, missing setup, noisy or different voices, missing/extra
key-ons, changing instrument setup, unexpected pitch intervals and any change
from the exact measured setup above. It also requires matching SGB1/SGB2 results.
Failures exit 2 without partial JSON. Success retains `qualification: false`
and `playback: false`.

ROM-free tests check bank routing, note/control bytes, checksum/hash pins,
transport bounds, no-overwrite CLI behavior, rejected/sanitized reference traces
and captured child failures. When the three private firmware files exist under
`roms/`, CMake registers an additional `local;private-reference` matrix gate.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(pitch|song_selection)' --output-on-failure
```

Only these two instruments, three notes, one channel and explicit controls are
covered. Other notes, transpose/fine tuning, instrument IDs, source mapping,
envelope timing, tempo/articulation, volume/pan curves, echo and multi-channel
scheduling remain open. No physical-device or independent-emulator comparison
was performed. The prototype renderer is unchanged and still rejects vendor
banks; this evidence does not enable commercial-title playback. An independently
owned sample strategy remains necessary for a distributable replacement.

The subsequent [tempo/articulation fixture](sgb-tempo-articulation-fixtures.md)
measures onset and key-off intervals with isolated control changes on channel 2.

The subsequent [chromatic octave measurements and native renderer](sgb-native-score-chromatic.md)
pin instrument-2 pitch words for base notes 24..36 on both voices and both
original models. Instrument 10 and general tuning remain outside that extension.

The [register-only chromatic extension](sgb-instrument-chromatic-reference.md)
now measures instrument 10 on both voices for all notes 24..36, with fresh
instrument-2 controls on both original models. The three-note fixtures and
their checker remain unchanged. The extension defines the bounded tuning contract now implemented by the
[D7 diagnostic](sgb-native-score-instrument-tuning.md), without claiming sample
or audible equivalence.
