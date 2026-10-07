# Bounded symbolic vendor score scheduler

The controlled [phrase](sgb-phrase-fixtures.md) and
[subroutine](sgb-subroutine-fixtures.md) gates now constrain the execution order
needed by a future SPC renderer. `scripts/schedule_sgb_score.py` implements an
independently written, bounded execution oracle for those contracts. It reads
an explicit raw bank and phrase root and emits a timeline in **symbolic score
ticks**. It does not generate audio, execute firmware or change production
ownership/restart behavior.

```sh
python3 scripts/schedule_sgb_score.py /tmp/owned-bank.bin --phrase 0x2B10
```

## Supported execution profile

The supplied bank must contain 1..8192 bytes based at `$2B00`. The phrase root
is explicit; the tool does not guess a song directory. Pattern tables may use
channel 2 alone or channels 2/3 together. Zero ends the phrase; phrase control
words below `$0100`, including repeats, are rejected.

Each new pattern starts fresh duration/articulation parser state and requires
both values explicitly before a timed event. Supported syntax includes duration
bytes 1..127, articulation bytes 0..127, notes `$80..C7`, rest `$C9`, and raw
E0/E1/E5/E7/ED control events. Controls retain their operand metadata; the oracle
does not calculate instrument/source mapping, pan/volume curves, tempo-to-clock
conversion, envelopes or PCM. The broad byte ranges describe syntax, not a
qualification of every note, duration or control value against the reference.
Ties, transpose, percussion, echo and other commands fail closed.

EF has a little-endian target and count 1..3. One call context per channel
remembers target, return cursor and remaining total executions. A callee zero
repeats or returns instead of ending the pattern. Duration/articulation state
persists through calls and return; the qualified fixture directly checks
continuation duration reuse, not every control's inheritance. Nested/recursive
calls, empty bodies and counts outside 1..3 are rejected.

At each ready tick, the scheduler consumes zero-time controls/call operations
until a note, rest or track end. The first ending track closes the pattern and
starts the next one at that tick. Long timed events retain their authored
`duration`, but their effective `end_tick` is clipped to the boundary with
`truncated: true`. Pattern records report ended and truncated channel lists.
Unexecuted tails of truncated tracks are not decoded; this is an execution
oracle, not a whole-bank validator.

Same-tick track-end/new-note ordering has not been measured. Such inputs are
rejected rather than assigning an unqualified ordering. Multiple tracks ending
together are supported. Zero-length active patterns and missing initial
articulation/duration are rejected. A zero-root empty phrase returns an empty
timeline. Across-pattern duration/control inheritance remains unqualified;
this profile deliberately requires explicit timed-event setup in each pattern.

The GBS4 diagnostic barrier remains unchanged and waits for all tracks. It is a
separate authored format; its rule is not reused for vendor phrase execution.
The conservative score inventory also retains its EF scan frontiers.

## Reference-aligned fixture results, 2026-10-07

```sh
python3 scripts/check_sgb_scheduler_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The optional checker schedules the independently authored phrase/subroutine
fixtures, then runs their existing bounded private original-program observation
gates. It compares grouped note onsets with actual combined KON masks and the
limited instrument-2 pitch points already measured (notes 24/25/36 map to DSP
words 1068/1132/2140 in these fixtures). It does not infer a general instrument
or pitch table from those points.

| Fixture case | Symbolic grouped onset ticks | Song end tick |
| --- | --- | --- |
| phrase: short-first | 0, 16 | 32 |
| phrase: long-first | 0, 16 | 32 |
| phrase: both-long | 0, 32 | 48 |
| subroutine: once | 0, 16, 32, 48 | 64 |
| subroutine: twice | 0, 16, 32, 48, 64, 80 | 96 |
| subroutine: thrice | 0, 16, 32, 48, 64, 80, 96, 112 | 128 |

All six cases agreed on both models: twelve fresh original-program runs passed.
The two phrase voices key on together, and subroutine bodies repeat the requested
total number of times before their two-note caller continuation. The comparator
normalizes each reference onset interval to duration 16, using the preceding
measured 84000..90000-cycle phrase and 84000..92000-cycle subroutine ranges. An
oracle using the old wait-for-all barrier fails this comparison for unequal
tracks. This alignment checks onset order/spacing; symbolic song-end ticks and
clipped note ends are not separate DSP key-off or audible release measurements.

The report contains only own-fixture pattern metadata, grouped notes, symbolic
ticks, relative reference intervals and image hashes. Its private runner reuses
the existing 8000000-instruction, 180-second-per-child bounds, trace caps,
expected instruction-bound exit and captured-output sanitization. No private
RAM, samples, firmware instructions or PCM are forwarded or committed. Any
failure exits 2 without partial JSON. Both oracle and reference reports retain
`qualification: false` and `playback: false`.

The execution oracle has independent ceilings of 8192 read operations, 1024
emitted events, 64 patterns and 65536 symbolic ticks. Every executed pointer
read must fit the supplied bank. CLI input reads at most 8193 bytes so oversized
files are rejected without loading them fully. Ten ROM-free tests cover the
real authored phrase/call banks, long-note truncation, repeat/return duration,
rests, empty phrases, unsupported grammar/channels, nested/empty/bad calls,
ambiguous boundaries, unexecuted tails, work budgets, metadata-only CLI behavior
and deliberate reference-alignment regressions. CMake registers the ROM-free
gate and, when all three private firmware files exist under `roms/`, an optional
`local;private-reference` alignment gate.

```sh
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_(score_scheduler|scheduler_local_reference)$' --output-on-failure
```

The scheduler is not connected to the host or uploaded SPC driver. Native vendor
rendering, independently owned instrument/sample mapping, complete pitch/volume
curves, additional channels, unqualified controls and reset/save-load behavior
still need implementation and validation. No physical-device or independent
emulator comparison was performed. The prototype continues to reject vendor
banks; commercial-title playback remains unsupported.
