# Experimental gated native finite calls

The isolated `build_sgb_score_callgate.py` combines the validated
[native finite-call parser](sgb-native-score-calls.md) with the
[independent measured gate renderer](sgb-native-score-polygate.md).
It is not bundled or selected by production playback. SGB1/SGB2 program
ROMs remain required.

The native parser follows one nonnested `EF` call per track, executes its body
1..3 times, and continues at the byte after the call. Every referenced body,
repetition and continuation is validated before timer activation or audio.
Each expanded track has 1..8 timed events, stored in four 32-byte cache slots.
The existing native countdown/rehearsal scheduler preserves first-end clipping
and rejects ambiguous same-tick end/new-event boundaries before publication.
The bank remains 1..128 bytes with exactly two patterns and channels 2/3;
there are at most 32 events and 2032 ticks.

Audio accepts notes `$98`, `$99`, `$A4`, rests `$C9`, and articulations 63/127.
Notes use only the ten measured tempo/articulation/duration profiles from the
gate renderer; rests permit duration 2..127. Duration and articulation persist
into calls, between repetitions and on return. A changed duration without a
new articulation retains the prior value. No profile interpolation or general
articulation law is introduced. Unknown notes/profiles reject even in a later
repetition or a track that would be clipped.

The call parser's target, return cursor, repetition count, nonempty-body flag
and call-used flag move to direct-page `$7C..80`. Active gate counters and
lookup scratch retain `$72..7B`. This keeps call expansion separate from live
voice state. The native caches store duration/opcode at `$3100`, selected
gate pulses at `$3200` and actual inherited articulation at `$3300`, using
the same pair offsets within each 32-byte slot. Runtime loads these records;
it does not look up profiles during note setup.

The unchanged owned DSP setup uses source 0, independent square-wave BRR,
direct gain 127 and separate left/right voices 2/3. Repeats and returns
re-key only due voices, start their measured pulse counters at KON, and
preserve unaffected peer KOF/envelope state. Gate expiry releases only expired
voices. Pattern boundaries/completion cancel counters and release any still
active clipped notes. Original instruments, envelopes, acoustic controls and
PCM are not reproduced by this diagnostic setup.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_callgate.py --output /tmp/score-callgate.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_callgate_probe
ctest --test-dir build-dmg-firmware -R 'native_score_callgate$' --output-on-failure
```

The reproducible 2073-byte artifact has SHA-256
`a829b6cef80c8f0f093068c6d2702e31d3c54b2671860287d759f3e32c01d22c`.
The previous muted-call and gated polyphonic artifacts remain byte-identical.
The production APU CPU, bus, timers and DSP execute the native code through
the owned diagnostic IPL trampoline/direct RAM installation. This does not
qualify actual upload, whole-system boot or production integration.

The nine-case component suite checks both articulations for counts 1..3,
all ten gate profiles through eight-event tracks, articulation changes inside
bodies and inheritance on return, independent voice articulations at tempos
96/128/192, asynchronous returns, surviving peers and clipped gates. After
10000 SPC cycles from an expiry, an expired voice must be exactly silent while
its peer remains audible. Rests exercise the complete 32-event/2032-tick bound.
Invalid nested/second calls, unsupported late profiles, bad targets, event
overflow, every truncated fixture prefix and ambiguous boundaries reject
without events, key-ons or nonzero PCM.

Reset and cross-instance save/load must preserve every event, articulation,
actual KON/KOF edge, pending pulse count, completion time, peer check and owned
PCM fingerprint. Checkpoints cover native expansion, rehearsal and playback,
each completed event pair and each actual release. Completion cancels counters
and drains PCM to silence. All fourteen native score suites and both existing
phrase/subroutine fixture suites passed.

Private black-box comparison:

```sh
python3 scripts/check_sgb_score_callgate_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_callgate_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms --articulation 127
# Repeat with --articulation 63.
```

The owned finite-call fixtures retain their two-channel/two-pattern shape:
shared body `$98,$99` repeated 1..3 times, caller continuation `$A4,$98`, then
second pattern `$99,$A4`. All notes have duration 16 at tempo 96. The new fixture
option changes only articulation operands in the body and second pattern;
default articulation-127 fixture images remain byte-identical.

Twelve fresh private SGB1/SGB2 runs passed. Exact combined KON masks, pitches,
note counts and repeat/return/pattern sequences are mandatory. All 192
per-voice note releases were directly observed original KOF writes. These
equal-duration fixtures permit no missing-KOF handoff inference, including
at pattern transitions. Original onset intervals retain the 84000..92000
SPC-cycle fixture bounds and the 2048-cycle two-model agreement requirement.
Actual native DSP onset spacing and per-voice gate lengths must match within
the existing 4096-cycle allowance. Maximum differences were 3556 cycles for
DSP onset spacing and 2901 cycles for gates. No timing allowance was widened.

These private runs qualify only the measured tempo-96/duration-16 coupled
call shape and its two articulations. Other measured profiles and asynchronous
peer behavior have separate component evidence. No original PCM, physical
device or independent-emulator comparison was performed. Private execution
and temporary traces retain the bounded/sanitized reference workflow; only
register/timing metadata and hashes are exported.

Reports use `gbb-spc-score-callgate-v1` and `gbb-score-callgate-reference-v1`
with qualification and playback false. The bundled prototype remains
unchanged. The separate [measured chromatic renderer](sgb-native-score-chromatic.md)
now supports base notes 24..36 through gates, repeats and returns. Broader
phrase/channel grammar, instrument/control compatibility and whole-system
production integration remain ahead.
