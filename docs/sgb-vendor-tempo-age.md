# Live tempo adoption before selection

The [starting/live tempo experiment](sgb-vendor-tempo-change.md) showed that
selection follows the later authored tempo in its corpus. This follow-up moves
the E7 command to the fourth short note and varies the second request time,
reducing the time available for selection state to adopt the new tempo.

The authored fixture adds optional `change_after=3` / `--change-after 3` and
active-only `request_gap` / `--request-gap` values 16 or 20. The default change
position remains after the first note, and default request timing is unchanged.
All 48 original phase fixtures and 32 earlier crossed-tempo fixtures retain their
hashes. The score still plays duration-2 A4/A3/A4/A3 and a duration-96 A2 tail,
then selects the unchanged tempo-45 A3 song. The E7 precedes the fourth note.
Directories, instrument bindings and independently authored assets are unchanged.

The 32-case corpus crosses starting/live tempos 32 and 128, both models, two
request gaps and both schedules. The other schedule adds two frames to both
request controls and the existing 1536-spin delay before the second request.
Both unchanged-tempo controls execute E7 in the same later position. Payloads
are identical across request schedules and gaps for each tempo pair.

A preliminary nine-frame request interrupted original playback before the
change and tail were reached. Its note ownership guards rejected the capture;
it is excluded rather than relaxing those guards. The 16/20-frame corpus keeps
all four prefix notes and the tail before the second request. Every first song
must still be active when reselection occurs; the selected second song must
complete. Existing binding, gate/clear, directory/root/pattern/track, timer-read
and configuration checks remain mandatory.

The first interval uses the previously documented 4-ms startup allowance.
The next two intervals must fit the starting tempo within 2 ms; the final
short-note-to-tail interval must fit the live tempo within 2 ms. The shared phase
validator accepts an explicit fourth-note witness anchor for this experiment;
its existing second-note default and guards remain unchanged. One live interval
constrains tempo without uniquely identifying accumulator phase. No phase is
chosen using selection timing.

The fourth KON occurs after the authored E7. Its time to the second request is
reported as `new_tempo_note_age_ms`, a conservative lower bound on command age.
It is not an observation of the original instruction or software state. Each
changed-tempo case is compared with unchanged-tempo controls at the same model,
request gap and schedule. The report retains control separation and both fixed
2-ms comparison flags, with no fitted parameters or firmware qualification.

```sh
python3 scripts/check_sgb_vendor_tempo_age_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Captures use 300 million SNES clocks, a 240-second timeout and a 96-KiB output
bound, with four parallel immutable fixtures. Originals run opaquely; only
public DSP writes, authored-address timestamps and bounded timer metadata are
retained. No original instructions, PCs, private score/sample bytes, RAM snapshots
or PCM are exported. Failures suppress child diagnostics and partial JSON.
Reports retain `qualification: false` and program/fixture hashes. Private
reports remain local and outside public CI.

All 32 local original captures passed. All 16 changed-tempo contrasts matched
the live-tempo control within 0.907227 ms; none matched the starting-tempo
control within 2 ms. Starting-tempo errors were 11.092773–12.396484 ms, and
unchanged-control separation was 11.092773–12.404297 ms. Measured changed cases:

| Change | New-tempo note age (ms) | Directory-to-root wait (ms) |
| --- | ---: | ---: |
| 32 → 128 | 59.595215–213.524902 | 20.000977–20.454102 |
| 128 → 32 | 131.595215–285.530762 | 31.546875–32.407227 |

Maximum starting-prefix fit error was 0.468750 ms; maximum live-interval fit
error was 0.363281 ms. Timer configuration stayed `[1,16,-1,-1]`, with zero
configuration writes after the first request. Both request gaps and both
schedules support the same live-tempo result.

The longer gap increased observed note age by 50.726563–68.465820 ms across all
16 matched age pairs. Startup phase is not assumed identical across generated
images; the guard requires a measured increase beyond the 2-ms comparison
resolution, rather than asserting a fixed four-frame increase in note age.
This age-pair analysis was added to the captured report without executing the
originals again. It does not change any selection contrasts or fit outcomes.

These results extend live-tempo retention to a later command with confirmed
new-tempo playback roughly 60 ms before selection. They do not establish an
exact command-to-adoption latency or immediate adoption. The existing opt-in
firmware already retains the live tempo; no firmware change follows from this
experiment.

Public owned controls run the same 32 timelines against the unchanged opt-in
firmware, checking both tempo witnesses, live-tempo pacing and matched controls.
Four other-schedule cases cover both models and change directions at the shorter
request gap, comparing native/scalar PCM and timing while checking reset,
destination restoration, unread PCM and selection-wait/pulse snapshots. Two short
contracts cover fixture scope, bad witnesses, duplicate/missing controls and
sanitized failures. Both firmware image hashes remain unchanged; program
replacement and complete audio equivalence remain unfinished.

## Authored tempo operand observation

The opt-in native flag `--tempo-operands` now extends `--timer-writes` with
post-KON reads of address `$2B70`, the tempo operand in this independently
written late-E7 score. It retains at most 64 address/timestamp/request tuples;
overflow rejects the capture before JSON output. It reports no memory values,
original PCs, instructions or RAM snapshots. The ordinary traversal observer
keeps its existing pre-KON bounds. Other modes remain unchanged.

The tempo-age gate uses this mode and emits schema
`gbb-sgb-vendor-tempo-age-v2`. The new `operand` object retains validated reads,
the last operand-read half-clock, its age at the second request, and its delay
to the first new-tempo note. Validation requires exact integer fields, the fixed
owned address, chronological order, valid request ownership and post-KON
capture bounds. Every request-1 operand read must lie strictly between the
third and fourth authored notes. Existing old/live tempo witnesses and selected
song completion guards still apply. A read timestamp is a bus observation;
it does not expose the original driver's software-state update instruction.

The existing 32-case public and private corpora use identical fixture bytes.
Four owned paired captures compare every earlier timeline field exactly with
the new mode enabled/disabled. Short contracts reject unexpected fields
(including memory values), wrong addresses, invalid ordering/ownership, absent
reads and count overflow. Existing host observer lifecycle contracts remain
applicable because this uses the existing SPC-read callback binding.

All 32 original captures contained exactly one operand read, strictly between
the third and fourth notes. The read preceded the first new-tempo KON by
1.6484375 ms in every case on both models. Measured operand age at selection
was 61.243652–287.179199 ms. Every earlier per-run summary and selection/age-pair
comparison matched the preceding 32-case report exactly. This supplies a direct
bus anchor for a future closer request sweep, without identifying the original
software-state update instruction or proving immediate tempo adoption.

Next, use the measured operand age to design a request sweep around the change.
Keep cases where the operand was read before the request separate from requests
that arrived while the old track was still pending; preserve explicit ownership
guards instead of inferring command timing from requested frame counts.
