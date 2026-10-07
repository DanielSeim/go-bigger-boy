# Experimental bounded native multi-event tracks

The isolated `score_multi.asm` program extends native raw-bank parsing and
muted scheduling to 1..4 timed events per track. It uses the same song root,
exactly two phrase patterns, eight-channel pointer tables with channels 2/3
active, and 1..128-byte bank bound as the
[native phrase parser](sgb-native-score-phrase.md). It is not bundled and does
not alter the [owned two-voice renderer](sgb-native-score-duet.md).
SGB1/SGB2 program ROMs remain required for production playback.

Each track starts with at most five setup commands from the earlier bounded
subset, then an explicit duration and articulation 127 before its first note
or rest. Duration is 1..127; notes are `$80..C7`, and rest is `$C9`.
Subsequent events can inherit duration/articulation, supply a new duration, or
supply both duration and articulation 127. Controls after timed events, other
articulations, ties, calls, empty streams and a fifth event are rejected.
The complete grammar and every referenced pointer are checked before execution.

The native parser writes four fixed 16-byte RAM cache slots at `$3100`, one per
pattern/track. Each contains up to four `(duration, opcode)` pairs and a zero
terminator. The host installs the raw bank, not a parsed timeline. The scheduler
keeps separate cursors and duration countdowns for channels 2 and 3, emitting a
new timed event only when that track's countdown expires. A first track end
advances the pattern and clips the peer; both simultaneous ends also advance.

When one track ends on the same tick as its peer starts another timed event,
the ordering is not qualified by the reference. That bank must reject before
any live event. To enforce this, the native SPC rehearses the same bounded
countdown routines with emission disabled, after grammar validation and before
enabling the timer. This rehearsal uses no timer pulses or published score-tick
increments. It checks both patterns, then rewinds cursors/countdowns and starts
the real native clock. The rehearsal and actual run are each bounded by 16
possible events and 1016 logical ticks. This is not a host simulation callback.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_multi.py --output /tmp/score-multi.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_multi_probe
ctest --test-dir build-dmg-firmware -R 'native_score_multi$' --output-on-failure
```

The 745-byte reproducible artifact has SHA-256
`738341ce61c29b0492795c0fbedfe8dc96c3d102087e3a0634b9f2371617c055178`.
The production APU CPU, bus and timers execute owned source through the usual
owned IPL trampoline/direct RAM installation. DSP mute/reset remains set,
PCM stays silent and no mailbox readiness is published. Complete reset and
cross-instance save/load must reproduce each logical event and its physical
half-cycle timestamp, including snapshots during parsing, rehearsal and live
countdowns. Invalid banks must emit no events and finish with zero score ticks.

The seven-case ROM-free suite aligns actual native records with the independent
symbolic N-SPC scheduler. It covers the existing single-event phrase banks, new
two-event fixtures, asynchronous per-track events, inherited duration and rests,
relocated tables/streams, both tempos, all 16 events, the 1016-tick bound, equal
track ends, malformed later events, every truncated fixture prefix and ambiguous
end/new-event boundaries in either pattern. Reports use
`gbb-spc-score-multi-v1` and retain `qualification: false` and `playback: false`.

New private black-box fixtures:

```sh
python3 scripts/check_sgb_score_multi_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_multi_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

The owned `build_sgb_multi_fixture.py` banks use two first-pattern notes per
track. Both initial durations are 8; second durations are `(8,16)`, `(16,8)` or
`(16,16)`. Duration 8 is inherited where possible; changed duration preserves
articulation. The next pattern has one duration-16 note per track. The original
programs must produce three combined voice-2/3 key-ons with pitches
`(1068,1132)`, `(1132,1068)`, `(2140,2140)`. Native scheduling records must match
the independently parsed logical score, and both adjacent onset intervals must
agree with the private reference within 4096 SPC cycles. Reference models must
also agree within 2048 cycles. Native event-log timestamps are not DSP key-ons.

Six fresh SGB1/SGB2 runs passed, with maximum native/reference adjacent-interval
difference 3308 SPC cycles. The new first patterns advanced at ticks 16 or 24,
then completed after the final 16-tick pattern. All ten native score CTest suites
and the existing phrase-fixture suite passed; the bundled prototype
reproducibility check retained SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.
Only sanitized register/timing
metadata and hashes are exported; original code, samples and assets remain
external. Comparison reports use `gbb-score-multi-reference-v1` with qualification
and playback false. Original PCM, gate/release timing, broader control grammar,
whole-system boot and production integration remain outside this milestone.

The separate [independent owned renderer](sgb-native-score-poly.md) now re-keys
due voices while preserving sounding peers. Independently gated polyphonic
notes using measured profiles remain next.
