# Experimental native finite score calls

The isolated `score_calls.asm` parser extends the muted native
[multi-event scheduler](sgb-native-score-multi.md) with bounded `EF` calls.
It is not bundled or connected to the polyphonic audio renderer. Production
playback still requires the external SGB1/SGB2 program ROMs.

The raw bank remains at `$2B00`, limited to 1..128 bytes, with a root word,
exactly two pattern tables and active channels 2/3. Each track permits one
`EF low high count` call, with total callee executions 1..3. A callee's zero
terminator repeats or returns to the byte following the call. Its duration
and articulation remain in effect on return; they also persist between repeats.
Shared read-only bodies and relocated tables/streams are accepted. Calls can
precede the first timed event or follow a caller note. Articulation remains
127 in this isolated layer.

The native parser follows each target, validates every repetition and caller
continuation, and expands 1..8 timed events per track before enabling timer 0.
Four 32-byte slots at `$3100` contain duration/opcode pairs and a terminator;
no host parser installs an expanded timeline. The existing native countdown
and silent rehearsal routines then execute the caches. Pattern offsets are
0/64 and peer-track offsets are 32. First-ending tracks clip their peers;
same-tick track-end/peer-new-event ambiguity still rejects before publication.
The total bounds are 32 events and 2032 ticks.

Nested or recursive calls, a second call site in a track, counts outside 1..3,
empty bodies, controls inside bodies, post-note controls, invalid targets,
missing operands/terminators and a ninth expanded event reject silently.
Targets need the correct bank page and an in-range offset; every operand and
body read is checked. Pointer bounds do not imply a globally typed bank layout:
the independently decoded target stream must also satisfy this grammar.
The one-call limit and nonempty-body/event bound prevent unbounded expansion.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_calls.py --output /tmp/score-calls.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_calls_probe
ctest --test-dir build-dmg-firmware -R 'native_score_calls$' --output-on-failure
```

The reproducible 847-byte image has SHA-256
`c61eb152bd76968e8420554c50eacf98bcc6aaf67c844d9cc6b7dbe35f0fdfb3`.
The prior multi-event image and bundled prototype remain byte-identical.
The production APU CPU, bus and timers execute this owned native code through
the diagnostic IPL trampoline/direct RAM installation. DSP mute/reset stays
set, PCM stays silent and no mailbox readiness is published.

The eight-case component suite checks every repetition count at tempos 96/192,
both channels, inheritance into/from calls and through changing body durations,
rests, relocation, shared bodies, independent cursors, first-end clipping,
all 32 events and 2032 ticks, invalid later tracks and every truncated fixture
prefix. Full reset and cross-instance save/load must reproduce complete event
records and physical half-cycle timestamps, including checkpoints during
parsing, repetition expansion, rehearsal and live countdowns.
All thirteen native score suites and both phrase/subroutine fixture suites
passed after this change.

Private black-box comparison:

```sh
python3 scripts/check_sgb_score_calls_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_calls_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

The new owned 109-byte banks use two channels and two patterns. Both first
tracks set instrument 2, pan 10 and track volume 127. Channel 2 also sets song
volume 160 and tempo 96. Both call the same body, `16,127,$98,$99,0`, then
continue with `$A4,$98,0`, inheriting its duration. The second pattern contains
`16,127,$99,$A4,0` on both channels. Only the call count changes between cases.

| Case | Count | Native events | Completion tick | Owned cartridge SHA-256 |
| --- | --- | --- | --- | --- |
| once | 1 | 12 | 96 | `2d25b56ad45fefafccd2bb5ade857bbbe2ef69f84de3ea43d4a0946179e3cb86` |
| twice | 2 | 16 | 128 | `c5314342323b91352d9b2bd81f90199af3c9cee33bad68a7293d1e20c91bd767` |
| thrice | 3 | 20 | 160 | `d4495cf43024e8087b4de485ec13be71bb7271d85258d27faa87fbb9e588243c` |

Six fresh runs passed against the private SGB1/SGB2 programs and IPL. Each
original onset must select both voices with KON mask 12, SRCN 2, no noise, and
the exact expected pitch pair: `(1068,1068),(1132,1132)` repeated count times,
then `(2140,2140),(1068,1068),(1132,1132),(2140,2140)`. Exact note counts and
caller-return/pattern-transition sequences are mandatory. Adjacent original
KON intervals must be 84000..92000 SPC cycles; matching models must agree
within 2048. Native event-log intervals must agree within the previously used
4096-cycle allowance. Maximum observed native/reference difference was 3523
cycles; maximum model difference was 471.

Native event publication is compared to original DSP onsets, not equated with
native audio playback. Gates, PCM, acoustic controls and whole-system boot
remain unqualified here. Each private execution is bounded to 8000000 SNES
instructions and 180 seconds; traces are temporary and limited to 16 MiB and
fewer than 32768 rows. Only sanitized register/timing metadata and hashes are
exported. Schemas are `gbb-spc-score-calls-v1` and
`gbb-score-calls-reference-v1`, with qualification and playback false.

The separate [gated native call integration](sgb-native-score-callgate.md) now
preserves voice state and validates repetition/return key edges. This original
muted layer remains unchanged; broader grammar and production integration
are still outside both experiments.
