# Controlled vendor volume and pan observations

The preceding [tempo/articulation fixture](sgb-tempo-articulation-fixtures.md)
measures note scheduling. These fixtures isolate pan, song volume and track
volume with independently authored notes on channel 2.

```sh
python3 scripts/build_sgb_volume_pan_fixture.py --case pan --output /tmp/pan.gb
python3 scripts/build_sgb_volume_pan_fixture.py --case volume --output /tmp/volume.gb
python3 scripts/check_sgb_volume_pan_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Each cartridge uses the shared header/checksums, VRAM transfer and actual JOYP
packet code. Its 128-byte bank at `$2B00` has three song roots, each with one
pattern and only channel 2 populated. Every track explicitly sets E0 instrument
2, E1 pan, E5 song volume, ED track volume and E7 tempo 96, then duration 16,
articulation `$7F`, base note 24 (`$98`), a duration-1 rest and an end. SOUND
requests IDs 1, 2 and 3 sixteen frames apart. Each song restores all controls
before its note, so the track-volume comparison restores song volume after the
preceding reduced-song-volume request. The bank contains no copied score,
instrument table, sample or program bytes.

| Fixture | Request | Pan | Song volume | Track volume | Change from request 1 |
| --- | --- | --- | --- | --- | --- |
| pan | 1 | 10 | 160 | 127 | None |
| pan | 2 | 0 | 160 | 127 | Pan only |
| pan | 3 | 20 | 160 | 127 | Pan only |
| volume | 1 | 10 | 160 | 127 | None |
| volume | 2 | 10 | 80 | 127 | Song volume only |
| volume | 3 | 10 | 160 | 64 | Track volume only |

## Measured contract, 2026-10-07

Both private original program images produced identical voice-volume snapshots
using GBB's independent SGB1/SGB2 GB bootstrap and the private original SPC IPL.
Each nonzero KON selected physical DSP voice 2 (mask `$04`), SRCN 2 and pitch
1068, with that voice's noise bit clear. The snapshots retain the latest accepted
DSP VOLL/VOLR writes before each KON. All the measured volume bytes are positive
or zero; signed phase/inversion operands are not covered.

| Fixture | Request | VOLL (`$20`) | VOLR (`$21`) |
| --- | --- | --- | --- |
| pan | 1 | 7 | 7 |
| pan | 2 | 0 | 11 |
| pan | 3 | 11 | 0 |
| volume | 1 | 7 | 7 |
| volume | 2 | 1 | 1 |
| volume | 3 | 1 | 1 |

For these cases, pan 10 is centered, pan 0 writes only a right volume and pan 20
only a left volume. Endpoint volume 11 differs from center volume 7. Halving the
song operand or approximately halving the track operand produces volume 1,
not half of the baseline register value 7. These points constrain the future
renderer; they do not establish the complete pan/volume curve, rounding rules,
velocity composition or multiplication order.

This gate observes **voice register setup**, not audible loudness or PCM
energy. The original reference startup also changes master-volume registers;
master-volume fades, envelope/sample response, effective DSP output and post-KON
volume changes are outside the comparison. No original samples or PCM are
exported, and no physical-device or independent-emulator comparison was run.

| Independently authored fixture | SHA-256 |
| --- | --- |
| pan | `00d139e36d83e474560d35682252bb059f92527f17b916300d04261ae42b1da1` |
| volume | `ba62f8a1f83679d2f83dd704245ff23f5f3c09a0e33e293ba8590e22dd464b04` |

Reference program/IPL/bootstrap pins match the preceding milestones. The
checker reports image hashes, model, case, request/control values and only
selected note setup. It pins the exact VOLL/VOLR pairs above and requires
matching SGB1/SGB2 snapshots. Missing/extra KON writes, missing setup, unexpected
voice/source/noise/pitch, malformed or unordered DSP events, files above 16 MiB
and traces at the helper's 32768-row cap fail the gate.

Four runs are each bounded to 8000000 SNES instructions and a 180-second child
timeout, with the expected instruction-bound exit required. Raw CSVs can contain
private RAM diagnostics; those diagnostics are discarded. Trace files and child
stdout/stderr remain temporary and are never forwarded, including on failures.
The checker exits 2 without partial JSON on failure. Successful reports retain
`qualification: false` and `playback: false`.

ROM-free tests cover the eight-channel tables, note/control stream, transport
bounds, header checksums and fixture hash pins, isolated operand changes,
strict/sanitized DSP observations, captured child failures and CLI no-overwrite
behavior. When the three private firmware files exist under `roms/`, CMake adds
a separate `local;private-reference` matrix gate; public CI needs no private ROMs.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(volume_pan|timing|pitch|song_selection)' --output-on-failure
```

Only the listed operands, instrument 2, base note 24 and one channel are covered.
Volume/pan fades, velocity mapping, signed pan flags, other curve points, master
volume, echo, multi-channel scheduling and independently owned sample mapping
remain open. The prototype renderer is unchanged and still rejects vendor banks;
commercial-title playback remains unsupported.
