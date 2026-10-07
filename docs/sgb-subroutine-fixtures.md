# Controlled vendor finite subroutine observations

The [two-channel phrase fixtures](sgb-phrase-fixtures.md) establish a bounded
pattern-transition contract. This fixture isolates finite EF subroutine calls,
repetition and return to a distinguishable caller continuation.

```sh
python3 scripts/build_sgb_subroutine_fixture.py --case once --output /tmp/call-once.gb
python3 scripts/build_sgb_subroutine_fixture.py --case twice --output /tmp/call-twice.gb
python3 scripts/build_sgb_subroutine_fixture.py --case thrice --output /tmp/call-thrice.gb
python3 scripts/check_sgb_subroutine_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Each independently authored cartridge uses the shared header/checksums, VRAM
transport and actual JOYP code. The 69-byte bank at `$2B00` contains one phrase
root at `$2B10`, one pattern at `$2B14` and a channel-2 caller at `$2B24`. Other
channels are zero. The caller sets instrument 2, pan 10, song volume 160, track
volume 127 and tempo 96, then emits `EF 40 2B count`, notes `$A4`, `$98` and end `$00`.
The callee at `$2B40` contains `10 7F 98 99 00`: duration 16, articulation `$7F`,
base notes 24 and 25, then return. The caller continuation is base notes 36 and 24 and
sets no duration/articulation of its own. Only the call-count operand changes
between cases 1, 2 and 3. One SOUND request, music ID 1, starts the entire song.
No copied score, sample, instrument table or firmware instructions are included.

## Measured contract, 2026-10-07

All three cases passed against both private original program images and the
private original SPC IPL with GBB's independent SGB1/SGB2 GB bootstrap. Every
nonzero KON selected physical DSP voice 2 (mask `$04`), SRCN 2 and no noise.
The observed DSP pitch sequence was:

| Case / EF count | Base-note sequence | DSP pitch sequence |
| --- | --- | --- |
| once / 1 | 24, 25, 36, 24 | 1068, 1132, 2140, 1068 |
| twice / 2 | 24, 25, 24, 25, 36, 24 | 1068, 1132, 1068, 1132, 2140, 1068 |
| thrice / 3 | 24, 25, 24, 25, 24, 25, 36, 24 | 1068, 1132, 1068, 1132, 1068, 1132, 2140, 1068 |

For these counts, the operand means **total callee executions**, not additional
repeats. The callee's zero terminator returns/repeats; the distinct note-36 pitch
followed by note 24 confirms return to the instructions following EF. The caller
sets no duration. Spacing from continuation note 36 to note 24 directly checks
that the continuation retains duration 16 from the callee.
This constrains duration reuse across return. It does not prove articulation,
volume, transpose or every other control's inheritance or restoration rule.

| Model | Case | SPC cycles between consecutive KON writes |
| --- | --- | --- |
| SGB1 | once | 86391, 87684, 85929 |
| SGB2 | once | 85935, 88147, 85928 |
| SGB1 | twice | 86391, 87789, 85825, 88618, 87512 |
| SGB2 | twice | 85935, 88252, 85824, 88154, 87975 |
| SGB1 | thrice | 86391, 87789, 85825, 88715, 87416, 86101, 88438 |
| SGB2 | thrice | 85927, 88260, 85824, 88251, 87879, 86100, 87974 |

These are relative SPC execution cycles between accepted DSP writes, not PCM
onset measurements. The gate accepts each interval in 84000..92000 cycles and
allows matching model intervals to differ by at most 2048 cycles. Those are
explicit bounds around the observed execution, not a universal tick size or an
exact EF overhead model. Exact pitch sequences and note counts are mandatory.

| Independently authored fixture | SHA-256 |
| --- | --- |
| once | `17a68151d643926e8bf0e9ee96c52bf93a8db7f2f6e93f4aeb65549a663b8e4b` |
| twice | `a71dc5d39c2c792a1f960a2ae5ccdab256f05678a9702cd130a12007ff8d420e` |
| thrice | `a4a35d67d7d2c75bf941528effa57e76d19611eee85338fa9adfc058ea775638` |

Reference program/IPL/bootstrap pins match preceding milestones and are printed
with fixture hashes. Six runs are each bounded to 8000000 SNES instructions and
a 180-second child timeout. The expected instruction-bound exit is mandatory.
The odd bank length and unaligned final transport header are exercised by the
actual private upload path, as well as ROM-free structural checks.

The observer retains only selected voice/source/pitch metadata and relative
intervals. RAM diagnostics in the raw CSV are ignored. Trace files and captured
child stdout/stderr remain temporary and are never forwarded. No sample or PCM
files are exported. Malformed/unordered DSP events, incomplete setup, wrong
voice/source/noise/pitch, absent key-ons, more than eight key-ons, wrong
repeat/return sequences, wrong timing, files above 16 MiB and traces at the
32768-row cap fail the gate. Failure exits 2 without partial JSON. Success
retains `qualification: false` and `playback: false`.

ROM-free tests cover call target/counts, body and caller continuation, complete
pattern/transport structure, checksums, hash pins and isolated count changes;
exact repetition/return and timing checks; bounded/malformed DSP traces;
child-output sanitization and CLI no-overwrite behavior. When all three private
firmware files exist under `roms/`, CMake registers a separate
`local;private-reference` matrix gate.

```sh
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_subroutine_(fixture|local_reference)$' --output-on-failure
```

Only counts 1..3, one call site, a two-note callee and channel 2 are covered.
Count zero, higher counts, nested/recursive calls, repeated call sites, malformed
targets, calls across channels/pattern boundaries, ties/rests and control
inheritance remain open. The conservative score inventory still stops at EF
frontiers; this gate does not make arbitrary private bodies safe to expand or
qualify larger-title playback. No physical-device or independent-emulator
comparison was performed. The prototype renderer is unchanged and still rejects
vendor banks; commercial-title playback remains unsupported.

The subsequent [symbolic scheduler](sgb-vendor-score-scheduler.md) executes a
bounded call/phrase profile and aligns its own-fixture timelines with these
original-program observations. The separate [native finite-call scheduler](sgb-native-score-calls.md)
now validates counts 1..3 on two channels and two patterns with new owned
fixtures; it remains muted and outside native audio integration.
