# Experimental executed boundary events before pattern overwrite

The subsequent [pending-note observations](sgb-score-pending-observations.md)
measure a following pattern with channel 2 inactive; those cases still reject
in this renderer. A separate [pending-note renderer](sgb-native-score-pending.md)
implements the measured inactive-pattern lifecycle.

The isolated renderer now executes a ready channel-2 note or rest before a
same-tick channel-3 end. Its controls and timing become inherited state, and a
note writes its pitch and volume. The next pattern starts channel 2 with another
note, replacing that pitch and pending gate before the single actual KON.
This extends [channel-2-first end priority](sgb-native-score-order.md), whose
source, image and conservative reverse-case rejection remain unchanged.

## Native execution and scope

`build_sgb_score_reverse.py` composes independently authored, checked source
hooks. When channel 3 ends and channel 2 is ready, a guard requires another
pattern with channel 2 active and its first event a note. The runtime clears the
transient KON mask/register without adding KOF or canceling running counters,
executes channel 2's event, applies channel 3's ending controls and advances the
pattern. Pattern startup replaces channel 2's setup and pending gate; only then
is KON issued. A boundary rest updates timing/mix and logs its execution without
pitch, volume, KON or a newly armed gate.

The timing-carry walker now loads a ready channel-2 event before checking for a
channel-3 end. It still stops immediately at a channel-2 end. This lets a later
track with omitted timing inherit the boundary event's duration/articulation,
even though that event never became an independently keyed note. Mix inheritance
already follows runtime execution order and now sees the boundary ED64.

The guard runs in the complete-bank silent rehearsal. A following pattern with
channel 2 inactive, a following channel-2 rest, or final termination rejects
before live records, DSP writes or audio. These cases require separate measured
contracts. The current raw comparison fixtures use a following solo channel-2
pattern; the guard also permits a following paired pattern that immediately
replaces channel 2's note. Broader held-voice and instrument interactions remain
unqualified. No general eight-channel grammar or production integration is added.

The program is 4050 bytes, ending at $17D1, SHA-256
`deece9488c7138fb4e9f0cde22beccf2edd3770f42b1df2a257f9b387cd15dda`.
The helper at $17B0 uses the existing space below the 4096-byte diagnostic cap.
The owned sample/instrument, 2048-byte source bound, four-pattern/eight-event
track caches, 64-event log and 2032-tick bounds are unchanged. Prior builders and
the bundled prototype retain their hashes.

## Raw execution versus physical key edges

The new `gbb-spc-score-reverse-v1` report retains every raw event, including the
boundary event, with an `event_patterns` array. This prevents a tick shared by
two patterns from hiding execution order. Actual accepted SPC writes to voice
VOL/PITCH registers are observed through the existing write-cycle callback and
exported as `voice_writes`, bounded to 256 entries. Each non-rest event must have
its exact four volume/pitch writes between that record and the next record;
rests must have none. The observed order is volume L/R followed by pitch low/high.

The independent symbolic scheduler has an opt-in `boundary_events=True` mode,
requiring `end_priority=True`. It marks an executed timed event at a first-end
boundary and includes its pattern ID. Its default and preceding callers retain
conservative rejection. Raw native events, timing, mix, controls and pattern IDs
must match this full oracle, including the boundary event's inherited changes.

Physical KON/gate validation uses a separate view that omits only an explicitly
validated boundary event immediately overwritten by the next pattern's
channel-2 note at the same tick. That view leaves all timestamps and key edges
unchanged. It does not create a release, envelope or KON for an unkeyed event.
The raw record remains in the exported report and in symbolic/write validation.
A key edge between it and its replacement record is forbidden. The existing
physical gate, envelope, PCM and lifecycle validators then check the actual
sounding notes. This separates event execution from note onset without allowing
arbitrary records to disappear from validation.

The probe saves/restores after complete pitch writes, including the transient
pitch before its replacement or KON, as well as its existing record, timer,
instruction and gate boundaries. Same-engine rewind and cross-engine continuation
must reproduce physical state and PCM. The baseline, restored and cold-reset
reports compare pattern IDs and every observed voice write exactly.

## Owned original-reference matrix

Six owned cartridges run at initial articulation 63/127 on SGB1/SGB2, producing
24 fresh bounded original executions and 24 native comparisons:

- `end-2-note` and `end-2-rest` retain the preceding skip behavior.
- `end-3-note` executes ED64, duration 8/articulation 63 and note A0 at tick 32,
  then replaces it with the next pattern's explicitly timed note 9A.
- `end-3-rest` executes the same controls and timing with C9, then starts note 9A.
- `inherit-note` and `inherit-rest` omit the next pattern's timing bytes. Its
  two notes inherit duration 8/articulation 63 and track volume 64.

The explicit cases have KON ticks 0/16/32/48/64/80/96/112. The inherited cases
have 0/16/32/40/48/64/80/96. All have eight actual KON groups and twelve actual
KOF releases. Reverse cases have thirteen executed records but only twelve
sounding notes/envelopes; skip cases have twelve records. Between KON groups 1
and 2, the note cases expose pitch 1700 then 1200, while the rest/skip cases
expose only 1200. No KON is attributed to pitch 1700. The observer also exports each boundary
pitch write's physical distance from the following KON, bounded to 4096 SPC
cycles. Native distances are compared directly with unchanged timestamps and
the existing 4096-cycle allowance; both originals must agree within 2048.
Maximum native/original boundary pitch timing difference is 1531 SPC cycles.
For SGB1 at initial articulation 127, the explicit note writes pitch 1700
2830 cycles before KON, then pitch 1200 at 1069 cycles before it. With inherited
timing those distances are 2718 and 1069; the rest writes only pitch 1200 at
1069 cycles before KON. Returning notes inherit
ED64 as volume 1/1, including the duration/articulation changes in inherited cases.

All 288 reference releases match native voice/onset identities and timer cause.
Maximum raw gate difference is 3460 SPC cycles; maximum onset interval difference
is 2987; maximum intermodel onset interval difference is 55. The 4096-cycle
native/original and 2048-cycle intermodel onset allowances are unchanged. Original
interval bounds remain 84000..92000 cycles per 16 authored score ticks, or half
those bounds for duration 8. No timestamps are shifted, timer restarted, score
pulses discarded or delays added to fit the evidence.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_reverse_probe
ctest --test-dir build-dmg-firmware -R 'native_score_reverse$' --output-on-failure
python3 scripts/check_sgb_score_reverse_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_reverse_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-reverse-reference.json
```

Original-only measurement may omit `--probe`. Each original child retains the
8-million-instruction, 180-second, 16 MiB trace and fewer-than-32768-row bounds.
The report schema is `gbb-score-reverse-reference-v1`. Only sanitized owned-voice
DSP metadata and input hashes are exported. Original instructions, scores,
instrument tables, samples and PCM are not inspected or copied. The 11-test suite
covers the fixture matrix, raw unkeyed writes versus actual edges, implicit carry,
guards, a deliberately reverted timing walker, all ten profiles and the full
64-event log, preceding peer lifecycle, oracle opt-in/default behavior, and false
pattern/event/write/key/reference metadata. All 11 new tests and 29 preceding
score, fixture and scheduler registrations passed (30 CTest registrations total),
along with the bundled-image reproducibility check and `git diff --check`.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic with qualification
and playback false. It is outside bundled or production selection. SGB1/SGB2
program ROMs remain required, and original PCM equivalence, physical hardware
and real-title replacement qualification remain open.

Next, measure the same boundary with a following pattern that leaves channel 2
inactive. Determine whether its accumulated note receives KON with the next
pattern's channel-3 note, and whether its new gate counts down or freezes. That
measurement must precede relaxing the current guard.
