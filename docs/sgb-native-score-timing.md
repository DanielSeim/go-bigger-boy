# Experimental duration and articulation inheritance

The subsequent [trailing-control diagnostic](sgb-native-score-tail.md)
qualifies ending-channel mix commands and documents a clipped-peer release gap.

The isolated native renderer now retains duration and articulation separately
for channels 2 and 3 across pattern boundaries and inactive patterns. Opaque
execution of owned fixtures on both originals confirms this behavior. A later
track may omit both timing bytes, or supply a new duration while retaining its
channel's articulation. Explicit duration/articulation pairs still override
both. The preceding [mix-inheritance renderer](sgb-native-score-inherit.md)
continues to build with its original hash and behavior.

## Bounded implementation

The parser seeds each active track from per-channel timing state: direct-page
`$B1/$B2` hold duration and `$B3/$B4` hold articulation. These begin at zero;
missing initial setup remains unqualified and rejects. Finite calls and repeat
bodies preserve the current track's state through the existing parser path.
Notes and rests establish timing state equally.

Whole-track expansion can parse timing changes in an unplayed tail. Simply
saving the final parser values would apply these changes to later patterns.
After expanding each complete pattern, a small independent cache walker follows
its active tracks in score-tick order. It updates carry only when a timed event
is reached and stops at the first active end, before advancing any peer at that
boundary. Inactive channels retain their previous values. The walker uses
`$32/$33` and `$61/$62` as temporary countdowns/cursors before the complete-bank
silent rehearsal initializes the real scheduler.

Each active cache contains one to eight events and a zero terminator. Durations
are bounded to 2..127, so a pattern's walk terminates in at most 1016 iterations.
The walker does not read uploaded instructions, write DSP registers, advance the
public clock, modify caches or source, restart timers, or publish events.
The existing silent rehearsal still rejects ambiguous simultaneous end/note
ordering and enforces the global 2032-tick limit before live playback.

Inherited values go through the existing measured gate-profile checks during
expansion. Unmeasured duration/articulation/tempo combinations reject before
any live DSP setup or audio. The existing ten gate profiles, 2048-byte source,
four patterns, eight expanded events per track, 64-event log, finite calls,
owned mix/envelope/source points and physical probe limits remain unchanged.
Trailing controls and timing changes without a following timed event remain
unsupported. This milestone does not qualify arbitrary first-end clipping or
zero-time boundary order against the originals.

The independent symbolic scheduler adds an explicit `inherit_timing=True`
option. Its default retains the preceding requirement for explicit setup in
each pattern. It tracks timing through executed raw score events, independently
of the native cache walker. The timing checker compares duration, articulation,
pattern boundaries, active masks, actual gates, instrument writes and mix state
against this oracle. Existing checkers retain their default behavior.

`build_sgb_score_timing.py` adds checked hooks and independently written source
at `$1610` and `$1700`, retaining the owned sample and descriptor. The strict
assembler checks placement and operands. The reproducible 3983-byte image ends
at `$178E` and has SHA-256
`1b6d47611ec2b81c0e07897cd24ecacfd57587afd6d8a887b681ce59d82b4dc3`.
All preceding images and the bundled prototype remain unchanged.

```sh
python3 scripts/build_sgb_score_timing.py --output /tmp/score-timing.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_timing_probe
ctest --test-dir build-dmg-firmware -R 'native_score_timing$' --output-on-failure
```

## Opaque reference evidence

`build_sgb_score_timing_fixture.py` supplies two owned 2048-byte banks. Both have
eight onsets with masks `12,4,8,12`, starts at ticks `0,32,64,96`, duration 16,
and centered full-volume instrument 2. Channels initially receive opposite
articulations (63/127 or 127/63). Channel 2 explicitly changes articulation in
the final paired pattern; channel 3 resumes its earlier state:

- `timing-hold` omits duration and articulation in later tracks.
- `art-hold` supplies duration 16 but omits articulation in later tracks.

| Case | Channel-2 initial articulation | Cartridge SHA-256 |
| --- | --- | --- |
| timing-hold | 127 | `196ecb7e4bb572bb42b2a05e37a77148f49242b29428b50b92549fd8f65749e1` |
| timing-hold | 63 | `169db99c5dca4f307ad692a104f948a8f1a14a36924bb5c3ed47517fd56705d4` |
| art-hold | 127 | `88eaa580cf157731ba2530695e10d1954c4ff81af2faf91b89aca992866fd972` |
| art-hold | 63 | `ebb394c50c3d73aa965ea101a842382ecff0d8a1fb923ea505175595ac452ae0` |

Eight fresh executions cover both cases, articulation assignments and SGB/SGB2.
All 96 per-voice releases are directly observed KOF edges. Pitch, source,
envelope and volumes match the owned onset contracts. Native/original maximum
gate difference is 3455 SPC cycles; maximum onset interval difference is 2810.
The existing 4096-cycle allowances and original 84000..92000-cycle onset bounds
are unchanged. Maximum intermodel onset difference is 55 cycles, below the
retained 2048-cycle allowance. No timestamps or timer phase are adjusted and no
artificial delays are added to fit these results.

```sh
python3 scripts/check_sgb_score_timing_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_timing_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-timing-reference.json
```

The checker can omit `--probe` for original-only measurement. Each original
child retains the eight-million-instruction, 180-second, 16 MiB trace and
32768-row bounds. Reports contain sanitized DSP/timing metadata and hashes,
not private instructions, scores, instrument tables, samples or PCM. The local
checks require the caller's originals and do not establish PCM equivalence.
Schemas are `gbb-spc-score-timing-v1` and `gbb-score-timing-reference-v1`.

The 15-test suite covers channel-specific timing, inactive reactivation, partial
overrides, rests, clipped tails, calls/repeats, all ten gate profiles alongside
inherited mix and actual instrument-prefix writes, full cache/log bounds, the
2032-tick limit, silent rejection, an omitted-walker fault and strict reference
metadata/timing mutations. The shared probe checks baseline, restored, reset
and cross-engine continuation; actual DSP edges/writes, ENVX, source/cache
guards and owned PCM must match throughout. All 15 new tests and the 25
preceding score/fixture/scheduler suites passed (26 CTest registrations total),
as did the bundled-image reproducibility check and `git diff --check`.

This remains an opt-in diagnostic with qualification and playback false. It
is not bundled or production-selected. SGB1/SGB2 program ROMs remain required.
The subsequent diagnostic measures trailing mix controls and end priority.
Clipped-peer release and simultaneous end/note ordering need further qualification.
