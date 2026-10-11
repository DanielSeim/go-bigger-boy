# Preceding-song tempo and score-root consumption

The [request-frame sweep](sgb-vendor-request-phase.md) retains a 25–28 ms
wait before root traversal on later original selections, with SGB1's first
selection near 48 ms. This experiment changes the preceding song's tempo
while retaining the newly selected song, and adds a previously unmeasured
request spacing. No firmware image, dispatch pacing, production selection,
emulator scheduling or performance policy changes.

## Authored controls

Song 1's tempo operand is either 45 (baseline) or 96. Songs 2 and 3 retain
tempo 45, the same distinct pitches, durations, articulation and instruments.
The request sequence stays 1, 2, 3:

- Selection 1 has no authored preceding song. Its resident initial tempo is
  unknown; do not substitute the first song's tempo for that state.
- Selection 2 follows authored tempo 45 or 96 and selects the unchanged song 2.
- Selection 3 follows authored tempo 45 in song 2 and selects unchanged song 3.
  It provides a return control, without assuming that all state has cleared.

Both models run active and completed playback with zero effects and both
STOP fields. `baseline` retains waits of 16/16/16 frames (active) or
16/100/100 (completed). `spacing-3` retains the first wait and adds three
frames to later waits: 16/19/19 or 16/103/103. Those spacings were unmeasured
in the preceding sweep; they are controls, not validation of a fitted policy.
There are sixteen fixtures and 64 private captures across originals and the
replacement on both models.

The existing dispatch fixture builder gains a bounded three-element
`track_tempos` tuple, defaulting to `(45,45,45)`. Its preceding outputs stay
byte-identical. Within each matched pair, tempo 96 changes only the score's
first-track tempo operand at `$2B69` (cartridge offset `$406D`) and the derived
global cartridge checksum. No GB wait code, asset payload, other track byte,
pattern or directory byte changes. The tempo-45 baseline schedule matches the
previous handoff/request-phase fixture exactly.

## Observation and report

The existing `--selection-traversal` mode and all note/setup, active/completed,
request ownership, directory/root/pattern/track and final-release gates remain
in use. The checker exports authored track tempos, explicitly labeled authored
preceding tempos (`null`, varied tempo, 45), requested frame counts, measured
delivery gaps and bounded traversal observations. Per-selection deltas use the
tempo-45 baseline of the same model, schedule, STOP profile and playback state.
A filtered tempo-96 run always includes that matching baseline. Comparisons
reject mismatched frame schedules, wrong baseline tempos and changed note IDs.

Reports use `gbb-sgb-vendor-prior-tempo-v1`, `qualification: false`, and image
identities. Times use the preceding 2048-half-clock/ms scale. Frame counts
remain authored wait controls; actual packet delivery can shift nonuniformly.
Original program images execute opaquely. Limits remain sixteen requests,
128 DSP events, 128 directory reads, 512 traversal reads, 64-KiB child stdout,
260 million master clocks and 240 seconds per private capture. No new native
observer, read-value export, private PC/instruction trace, RAM snapshot or PCM
export is introduced.

## Reproduce

```sh
python3 tests/sgb_vendor_prior_tempo_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_vendor_music_probe \
  --timeline-probe build-dmg-firmware/gameboy_sgb_music_timeline_probe
python3 scripts/check_sgb_vendor_prior_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms
```

The public full matrix checks 32 observed owned timelines against 32 ordinary
controls. Four tempo-96 replay cases use the new three-frame spacing, both STOP
fields, both models and both playback states, requiring exact reset and
repeated destination save/load PCM/state, including unread FIFO restoration.
The short contracts check deterministic fixtures, exact payload changes,
exclusive export, summary guards and filtered baseline retention using
only authored metadata. This does not establish acoustic, hardware,
independent-emulator or commercial sound-bank equivalence.

## Local findings

All 64 private captures pass. Each selection has its expected note/setup,
request ownership, traversal and playback state; all fixtures release finally.
All sixteen original-schedule tempo-45 captures retain the preceding sweep's
traversal measurements exactly.

Changing the preceding song's tempo reduces selection 2's directory-to-root
wait in every matched original comparison, including completed-song cases:

| Original model | Preceding tempo 45 | Preceding tempo 96 |
| --- | ---: | ---: |
| SGB1 | 25.466–26.008 ms | 18.002–20.412 ms |
| SGB2 | 25.516–26.424 ms | 18.005–20.007 ms |

These ranges include both schedules, STOP profiles and playback states. The
replacement stays at 0.131 ms for both tempos. The first-selection root wait
remains near 48 ms on SGB1 and 27.6 ms on SGB2; it is measured before the first
track consumes the changed tempo operand. On selection 3, after unchanged
tempo-45 song 2, the originals return to roughly 26–28.5 ms. This return is
not exact baseline equality: matched selection-3 waits move by up to 3.0 ms
on SGB1 and 1.586 ms on SGB2. Request phase and other state are still visible.

For example, SGB2's active baseline selection-2 wait moves from 25.602 to
20.004 ms; with the new three-frame spacing it moves from 26.405 to 19.598 ms.
In completed playback at that spacing, it moves from 26.000 to 18.005 ms.
Thus the preceding tempo can affect this transition after natural release,
not just while the earlier note is still sounding. These are directory-to-root
intervals; do not substitute them for the full SOUND-to-key-on latency.

The result rejects a single constant wait across these admitted tempos.
It also does not support simply scaling the whole interval inversely with
tempo: multiplying a roughly 26-ms wait by 45/96 would predict roughly 12 ms,
below every observed tempo-96 wait. No timer period, fade law or implementation
instruction sequence is inferred, and no pacing policy is introduced.

Next, observe bounded master-volume and channel-2 volume writes alongside
key-off, directory lookup and root traversal. That can distinguish an audible
transition/fade from an idle interval and identify which portion changes with
the preceding tempo. Retain the same authored tempo controls, completed cases
and previously unmeasured spacing. Additional tempo/phase holdouts, rapid
selection, STOP/upload ownership and pinned-title validation remain necessary
before a timing/fade policy is adopted.

## Validation evidence

The public fixture/report/timeline run passes four methods in 470.356 seconds,
including 32 observed captures and 32 identical ordinary controls. The separate
four-case tempo-96 reset/save-load replay passes in 108.716 seconds, preserving
exact PCM/state and unread-buffer restoration on both models and playback
states. Ten focused native/report CTests pass in 6.95 seconds. Four shard-runner
tests pass; discovery registers the new matrix exactly once among 94 public
matrices and includes both probe targets. Python compilation and whitespace
checks pass.

The unchanged deterministic DB image is
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Original identities remain SGB1
`a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376`
and SGB2
`c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6`.
Original instructions, proprietary score bytes and assets were not read as
implementation inputs. Earlier broad matrices, pinned-title playback, Windows,
independent emulators and hardware were not rerun. Production selection and
performance policy stay unchanged.
