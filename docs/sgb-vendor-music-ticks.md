# Music-tick timing through authored repeated notes

The [persistent accumulator selection model](sgb-vendor-selection-timers.md#persistent-accumulator-prototype)
failed the root-wait accuracy bound. This experiment observes music sequencing
separately, using eight independently authored short notes and public DSP writes.
It changes no firmware, emulator core, assets in distribution or accuracy bound.

`build_sgb_vendor_tick_fixture.py` uploads the existing independently authored
instrument-2 descriptor and looping BRR fixture. Its first song alternates A4/A3
four times, with duration 2 and articulation 125. Six preceding-tempo points
(32, 45, 64, 96, 112, 128) are tested on both models. The baseline starts SOUND1
after 16 frames; the second schedule uses 18 frames plus the existing 1536-spin
subframe control. Uploaded payloads are identical between schedules. Only one
SOUND request is made; every one of its eight KON writes remains observable.

The checker uses the existing `--timer-continuity` probe. It validates request
ownership, all eight pitch/binding tuples, each note's KOF gate, the gate clears,
final completion/release and the bounded timer aggregates. Continuity ownership
is per request: its validator receives the first KON and all KOF events, while
the tick report retains every note onset. The final note can hold its KOF gate
until completion; that is recorded as a null separate gate-clear interval.
The original pilot clears that gate separately, whereas the owned candidate
holds it until completion. This difference is retained, not treated as matching
gate behavior. The owned controls gate onset/completion timing, not gate accuracy.

Observation remains limited to authored-address traversal, public registers and
zero/nonzero timer-read metadata. No private code, PCs, score/sample bytes, RAM
snapshots, exact timer output values or PCM are inspected or exported. Each
capture is bounded to 200 million SNES clocks, 240 seconds and 96 KiB, with four
captures concurrent and immutable fixture filenames. Missing/invalid captures
suppress child diagnostics and produce no partial report.

```sh
python3 scripts/check_sgb_vendor_tick_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The diagnostic model advances a modulo-256 accumulator by the authored tempo
per hypothesized 2-ms opportunity, retaining its fractional remainder between
notes. Expected onsets occur every two carries. Each model/tempo pair fits one
initial phase from its baseline onsets and freezes it for the other schedule.
Completion is checked separately and is not used to fit phase. An explicitly
labeled independent per-case fit is diagnostic only and cannot alter that frozen
comparison. Reports retain `qualification: false`.

A second diagnostic reanchors at the second KON and compares the remaining six
onsets, fitting its phase from the baseline alone. This separates the first
interval from subsequent sequencing. These schedules were observed before this
analysis, so the second schedule is held out only from phase-parameter fitting;
it is not an unseen corpus qualifying a new firmware implementation.

Final original evidence covers 24 captures, with these maximum onset errors:

| Anchor | Baseline | Other schedule with frozen phase |
| --- | --- | --- |
| First KON, seven subsequent onsets | 0.613281 ms | 4.159180 ms |
| Second KON, six subsequent onsets | 0.470703 ms | 1.985352 ms |

All twelve model/tempo pairs pass the unchanged 2-ms bound after reanchoring at
the second KON. With the first KON as anchor, ten of twelve other-schedule cases
pass. SGB1 tempo 128 has a first interval of 10.159180 ms versus 6.613281 ms in
the baseline. Even independently fitting its initial phase leaves a
2.159180-ms onset error; its frozen completion error is 2.310547 ms. SGB2 tempo
45 misses the frozen first-anchor bound by 0.146484 ms, but an independent
phase fit reduces its maximum onset error to 0.548828 ms. These are diagnostic
constraints, not evidence that the original stores the fitted phase value.

The results support a continuous fractional music clock for this short steady
sequence, while first-note timing remains a distinct issue. They do not explain
the selection delay, establish arbitrary-score timing, or qualify a replacement
scheduler. The existing five admitted waits and tempo-112 exclusion remain.

All fourteen short selection-wait methods pass, including fixture payload
identity, invalid controls, binding/ownership/gate/completion rejection, final
gate-clear representation, tempo deduplication, baseline-only phase fitting and
sanitized failure. Twenty-four owned controls pass their onset/completion bounds
on both models and start schedules. Python compilation, CMake configuration and
whitespace checks pass. This step does not rerun unrelated full matrices,
platform/sanitizer, audio-PCM, lifecycle, hardware or independent-emulator checks.

Next, add a short-note prefix followed by a long authored tail and a second song
request, in active and completed states. That will give a measured music-tick
phase before reselection and let us test its relationship to the selected-root
wait. Keep the current firmware until that relationship predicts independent
schedules and passes the direct timing and lifecycle gates.
