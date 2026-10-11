# Opt-in preceding-tempo selection pacing

This experiment implements a bounded directory-to-root wait in the independently
written DB firmware. It follows the [volume correction](sgb-vendor-volume.md)
and the earlier [preceding-tempo observations](sgb-vendor-prior-tempo.md).
The ordinary DB image and production firmware selection remain unchanged.
Enable the experiment explicitly when building an external diagnostic image:

```sh
python3 scripts/build_sgb_vendor_music.py --selection-wait --output /tmp/paced.rom
```

The candidate is not a recovered original scheduler or a general tempo law.
It has direct reference gates at five tempo points and deliberately leaves
other tempos and the first selection unpaced. Full SOUND-to-KON equivalence,
startup behavior, fades and commercial sound-bank compatibility remain open.

## Evidence and a rejected point

The expanded opaque-reference experiment varies the preceding song's tempo
through 32, 64, 112 and 128. Songs 2 and 3 remain at tempo 45 with distinct owned
notes and the same authored uploaded instrument/sample. Two request schedules
use 16/20-frame active waits and 100/104-frame completed waits. Completed
fixtures shorten only song 1's duration from 96 to 48 so tempo 32 has actually
completed before the next request. Both playback states are verified from
observed key-off transitions; frame counts alone do not classify them.

All 32 revised captures pass the existing ownership, pitch, envelope, traversal
and final-release guards. The earlier duration-96, tempo-32, 99-frame completed
control was rejected because the first song had not actually completed. It is
not used as completed-playback evidence.

| Preceding tempo | Original second-selection root wait | Candidate timer wait |
| --- | --- | --- |
| 32 | 31.596–32.454 ms | 32 ms |
| 45 | roughly 25.5–28.5 ms in earlier controls | 27 ms |
| 64 | 23.554–24.411 ms | 24 ms |
| 96 | roughly 18.0–20.4 ms in earlier controls | 19 ms |
| 112 | 16.403–20.011 ms in the revised controls | excluded |
| 128 | 20.003–20.454 ms | 20 ms |

Tempo 128's wait can exceed tempo 112's: a monotonic inverse-tempo rule is not
supported. More decisively, an additional, previously unmeasured two-frame
completed control at tempo 112 gives approximately 20.003 ms on SGB1 and
15.600 ms on SGB2. No single delay can cover that spread within the chosen
±2-ms error bound. A six-point candidate failed that independent gate; tempo
112 was removed instead of loosening the accuracy gate. The five-point
candidate retains the default provisional pacing at tempo 112 and at other
unlisted values. The remaining points still need broader phase/state evidence.

The large original first-selection wait remains separate: approximately 48 ms
on SGB1 and 27.6 ms on SGB2 in these fixtures. The candidate leaves first
selection timing provisional. It also does not reproduce the original host
polling delay, root-to-pitch setup time or DSP reset/release behavior. Matching
this one stage must not be described as matching complete selection latency.

## Implementation and ownership

`firmware/sgb/vendor_selection_wait.asm` is inserted only by the builder's
`selection_wait=True` option. The default build is byte-identical to the
preceding volume milestone. Both builds use the existing DB diagnostic protocol;
the optional image remains externally selected and is not bundled.

Only the last **live** E7 tempo updates the persistent prior-tempo byte `$7B`.
The whole-score validation pass cannot change it. After the selected directory
word is read during live playback, an admitted prior tempo starts timer 1 at
8 × 125 microseconds, with the table's millisecond count. Lookup and return
instructions add a small measured overhead; the timer count is not itself an
exact directory-to-root timestamp. The experimental full capture checks allow
0.4 ms for this overhead. Unsupported tempos scan the table and return without
a timer wait, within 0.4 ms in the public controls.

The loop calls the existing mailbox service while waiting. A newer music commit
resets the old playback stack and discards its pending root traversal. Explicit
music STOP, rejection, reload and cold start clear prior history and the wait
marker. Natural completion retains prior tempo, as required by the completed
reference controls. Reload disables the timer before entering the IPL.
No frame counts or emulator host-clock reads drive the firmware delay.

Scratch bytes `$7C/$7D/$7E` hold the active marker, remaining count and accumulated
timer pulses. Existing RAM, timer and CPU continuation serialization covers the
wait. The owned playback probe now counts destination restores while `$7C` is
active and triggers a restore when that marker changes and after elapsed timer
pulses decrement the remaining count; the extra diagnostic
counter is not part of emulator serialization.

Rapid reselection and explicit STOP fixtures cancel selection 2 before its KON.
The third song retains the last live tempo after rapid reselection, while STOP
clears it. The GB upload fixture reissues SOU_TRN for the already displayed,
owned asset frame and verifies history reset and subsequent uploaded-instrument
playback. In this end-to-end transport, the reload reaches the driver after
selection 2 has sounded, so it is not evidence of mid-wait upload cancellation.

A separate owned SPC mailbox probe tests that boundary directly: it runs the
actual assembled candidate, reaches its active wait, requests reload, and
requires return to the IPL within 1024 SPC cycles, with history/marker cleared,
timers disabled and no selection-2 KON. This does not bypass the transport in
the GB playback checks or claim to test the IPL transfer itself. Only owned
firmware RAM is inspected by this probe; private originals use the bounded,
register/address-only timeline probe.

## Direct gates and reproduction

The private checker captures each original, the normal DB image and the opt-in
candidate on identical owned fixtures. It requires, for selections 2 and 3:

- candidate root timing within 0.4 ms of its authored wait;
- absolute error against the original at most 2 ms;
- at least 14 ms less error than the normal DB image;
- unchanged first-selection pacing within 0.05 ms of the normal image.

It also retains the preceding ownership, active/completed, note setup,
read-window and final-release guards. Successful reports contain only bounded
stage times and hashes with `qualification: false` and `opt_in: true`.
Child errors are suppressed, and a failure produces no partial report.
Three independent captures per fixture run in parallel; their fixture file is
immutable until all children finish. Each child retains the 260-million-clock,
240-second and 64-KiB bounds.

```sh
python3 scripts/check_sgb_vendor_selection_wait_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
python3 scripts/check_sgb_vendor_selection_wait_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --state completed --request-offset 2
ctest --test-dir build-dmg-firmware \
  -R 'gameboy_sgb_(vendor_selection_wait_contract|score_vendor_selection_wait)$' \
  --output-on-failure
```

The default reference run has 60 captures across both models, five tempos and
active/completed playback with the four-frame offset. The separate two-frame
completed gate has 30 captures and independently tests the frozen five-point
policy. CLI tempo/state filters deduplicate cases; request offsets are bounded
to 0..4. Authored frame changes are not treated as exact physical arrival shifts.

The public matrix executes the candidate's five tempo points in both playback
states, tests unlisted tempos 99 and 112, compares native/scalar PCM, restores
during the wait and with unread audio, and exercises all three interruption
profiles. It also runs the direct mid-wait reload probe. The short build/error
contract remains in platform/sanitizer jobs. The full public matrix is labelled
`sgb-firmware-extended`; both optional reference gates are `local;private-reference`.

The normal DB image remains
`0455c511726ec625afee474a90eba960f5eae2e156b4772d6f7b82b338d36ffd`.
The five-point candidate is
`ac38ff5a6fe9a0ba95ca6055445535aba44f42137633b711f41b147b4e49df31`.
Original images are executed opaquely. No original instruction sequence,
proprietary score/table/sample bytes, RAM snapshot or PCM is exported or used
as implementation source. Performance optimization remains deferred.

Next, explain the phase/state dependence at tempo 112 and the distinct cold
first-selection interval before expanding or enabling the policy by default.
Root pacing alone does not qualify audio equivalence or a production ROM.
The [completed duration/phase follow-up](sgb-vendor-selection-phase.md) narrows
the tempo-112 variation toward request phase while retaining its exclusion.

## Local validation evidence

All 90 original/default/candidate captures pass: 60 four-frame active/completed
captures plus 30 independent two-frame completed captures. Maximum absolute
root-wait error is 1.677 ms in the first set and 1.659 ms in the independent
set; minimum improvement over the default image is 16.614 ms and 15.829 ms,
respectively. The candidate hash in both reports is the five-point hash above.

Seven focused native/report CTests pass, including the selection-wait builder
and error guards. The later added filter/sanitization contract also passes.
Four shard-runner contracts pass; discovery includes the new matrix once among
96 public matrices and finds all three probe targets. Default-build deterministic
export and real-DSP scalar/native/combined playback pass in 40.285 seconds.
The direct owned mid-wait reload probe returns to the IPL in 173 SPC cycles.
A focused candidate native/scalar pair preserves exact PCM and cold/reset/load
continuation with 59 elapsed-pulse restores and 62 active-wait restores per run,
including unread-buffer restoration.

Python compilation and whitespace checks pass. Windows, sanitizers, independent
emulators, physical hardware and the other broad matrices were not rerun locally.

The full public selection-wait matrix passes, including native/scalar replay,
rapid reselection, STOP, the GB upload sequence and the direct reload probe.
The pinned private Donkey Kong candidate gate passes on both models with exact
reset/destination-load continuation and unread buffers. This is bounded title
playback evidence, not original-sound equivalence.
