# Controlled vendor two-channel phrase transitions

The preceding [echo fixtures](sgb-echo-fixtures.md) pin selected register setup.
This next independently authored fixture tests two-channel scheduling and the
transition between patterns with unequal track durations.

```sh
python3 scripts/build_sgb_phrase_fixture.py --case short-first --output /tmp/phrase-short.gb
python3 scripts/build_sgb_phrase_fixture.py --case long-first --output /tmp/phrase-long.gb
python3 scripts/build_sgb_phrase_fixture.py --case both-long --output /tmp/phrase-control.gb
python3 scripts/check_sgb_phrase_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Each cartridge uses the existing header/checksums, VRAM transport and actual
JOYP code. The 108-byte bank at `$2B00` contains one song root at `$2B10`, pointing
to pattern tables `$2B20`, `$2B30`, then zero. Each eight-channel table populates
only channels 2 and 3. All tracks explicitly set instrument 2, pan 10 and track
volume 127. Song volume 160 and tempo 96 are set once on channel 2 of pattern 1.
Each track plays one note with articulation `$7F` followed immediately by track
end. One SOUND request (ID 1) starts the entire sequence; subsequent requests
cannot interrupt a pattern.

Pattern 1 plays base notes 24 and 25 on channels 2 and 3. Pattern 2 plays base
note 36 for duration 16 on both. Only pattern-1 duration operands change:

| Case | Channel 2 duration | Channel 3 duration |
| --- | --- | --- |
| short-first | 16 | 32 |
| long-first | 32 | 16 |
| both-long | 32 | 32 |

The fixtures contain no copied score, sample, instrument table or program bytes.
The bounded structural decoder also verifies both pattern tables; its local
track tick counts alone do not determine the reference's phrase transitions.

## Measured contract, 2026-10-07

All three cases passed against both private original program images and the
private original SPC IPL with GBB's independent SGB1/SGB2 GB bootstrap. Each
pattern produced one combined KON mask `$0C`, selecting physical voices 2 and 3.
Voice 2/3 pitches were 1068/1132 for pattern 1, and 2140/2140 for pattern 2.
All four voice setups used SRCN 2 with their noise bits clear.

| Model | Case | SPC cycles between the two pattern KON writes |
| --- | --- | --- |
| SGB1 | short-first | 87903 |
| SGB2 | short-first | 87447 |
| SGB1 | long-first | 88057 |
| SGB2 | long-first | 87601 |
| SGB1 | both-long | 175505 |
| SGB2 | both-long | 175504 |

The unequal cases transition near the duration-16 interval established by the
[timing fixture](sgb-tempo-articulation-fixtures.md); both duration-32 tracks
roughly double the interval. Swapping the short track preserves the early
transition. **For these two channels and patterns**, either track reaching its
end advances the phrase, replacing both pitches before the longer track's full
duration would elapse. This supports a first-ending-track rule, rather than a
barrier that waits for every active track. It does not identify the instruction
or exact tick on which the driver changes its pattern pointer.

The independent GBS4 diagnostic format deliberately waits for all tracks at a
phrase boundary. That authored contract is unchanged. A future vendor scheduler
must keep the newly measured transition behavior separate; reusing the GBS4
barrier would fail these native reference cases.

The regression gate accepts unequal-case intervals 84000..90000 and control
intervals 170000..180000 SPC cycles. Matching model intervals may differ by at
most 2048 cycles. These are chosen bounds around the execution evidence, not a
universal scheduler tick definition. Exact combined masks, per-voice source and
pitch sequences must match across models. Only relative accepted-DSP-write
cycles are retained; this is not PCM onset/release or waveform equivalence.

| Independently authored fixture | SHA-256 |
| --- | --- |
| short-first | `7d79c2c3f49d4cc7ba01755fcc0b4a667852c671bad681c24373d429bca0f922` |
| long-first | `34fe6ecb5b07e91b50cce483ebd8f81bb110551aeff12b87050d5c2d0250b4f6` |
| both-long | `48b1f9af9fc605864cce6d8a3f2d6882533d8d0f533d1ead5771ee560087efd6` |

Reference program/IPL/bootstrap pins match preceding milestones and are printed
with the fixture hashes. Six runs are each bounded to 8000000 SNES instructions
and a 180-second child timeout. The expected instruction-bound exit is mandatory.
Raw CSV RAM diagnostics are ignored; trace files and child stdout/stderr remain
temporary and are never forwarded. No sample or PCM files are exported.

The gate rejects missing/extra or split KON events, missing voice setup,
unexpected source/pitch/noise, malformed or unordered DSP writes, nonpositive
intervals, files above 16 MiB and traces at the helper's 32768-row cap. Failures
exit 2 without partial JSON. Success retains `qualification: false` and
`playback: false`.

ROM-free tests cover the complete two-pattern/four-track structure and controls,
duration orders, header checksums, transport bounds and hash pins, static decoder
acceptance, mask/pitch/timing regressions, strict/sanitized DSP observations,
captured child failures and CLI no-overwrite behavior. When all three private
firmware files exist under `roms/`, CMake adds a separate
`local;private-reference` matrix gate.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(phrase|echo|volume_pan|timing|pitch|song_selection)' --output-on-failure
```

Other channel pairs, all eight channels, silent/inactive tracks, rests/ties at
boundaries, zero-duration or empty tracks, phrase repeats, subroutines and
control inheritance remain open. No physical-device or independent-emulator
comparison was performed. The prototype renderer is unchanged and still rejects
vendor banks; commercial-title playback remains unsupported.
