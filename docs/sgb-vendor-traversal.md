# Authored score traversal before pitch setup

The [directory-read experiment](sgb-vendor-directory.md) leaves 28–50 ms
between the original program's selected directory lookup and expected pitch
setup. This extension observes the fixed authored root, pattern-table and
first-note addresses in that interval. No experimental firmware, production
selection, emulator scheduling or DSP arithmetic changes. Performance work
remains deferred.

## Observation contract

The eight unchanged dispatch fixtures select IDs 1, 2, 3 with distinct notes,
active/completed state, and four effect-STOP controls. The opt-in timeline
mode `--selection-traversal` includes all directory and handoff metadata and
adds `selection_traversal_reads`: request, physical APU half-clock and address
only. The fixed whitelist is:

| Authored structure | Observed addresses |
| --- | --- |
| Three root lists | `$2B10..$2B1B` |
| Three eight-channel pattern tables | `$2B20..$2B4F` |
| Track 1 through its first note | `$2B60..$2B6C` |
| Track 2 through its first note | `$2B90..$2B9C` |
| Track 3 through its first note | `$2BC0..$2BCC` |

All addresses and ownership derive from authored fixture construction. The
observer retains reads after audible SOUND delivery and before the first
key-on of each request; it never exports read values, PCs, instructions,
private score bytes, packets, RAM snapshots, assets or PCM. Repeated visits
and reads of other whitelisted structures remain visible. The separate
512-read cap fails on overflow. The private child stdout bound is 64 KiB for
this mode; preceding modes retain their 32/48-KiB bounds. Each private capture
retains the 260-million-master-clock bound and 240-second timeout.

The checker retains all directory, pitch, setup, request ownership and release
contracts. It rejects unexpected fields, bools, invalid requests, timestamps
outside request/key-on bounds, unordered reads and addresses outside the
whitelist. After the last selected-directory read before expected pitch setup,
it requires the selected root pointer bytes, channel-2 pattern pointer bytes,
and track start/first-note addresses in successive stages. Earlier validation
reads stay in the report. Each stage records the first read across both visits,
the first read after the preceding stage (`last_pass_first_read_ms`), its last
read before setup, its read count, and its delay after the preceding stage.
These are bounded memory-consumption milestones; no private instruction or
general command-acceptance boundary is inferred.

## Reproduce

```sh
python3 tests/sgb_vendor_traversal_tests.py \
  --timeline-probe build-dmg-firmware/gameboy_sgb_music_timeline_probe
python3 scripts/check_sgb_vendor_traversal_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms
```

The optional original comparison executes local original images opaquely and
reports `qualification: false` with fixture/program identities. The shared
capture runner keeps the preceding handoff and directory schemas and flags;
a public contract exercises all three modes with authored metadata only.
Passing these controls does not establish commercial sound-bank, acoustic,
independent-emulator or physical-hardware equivalence.

## Local findings

All 32 private captures pass (eight fixtures, two models, original/replacement).
Every original selection has 31 whitelisted reads before key-on: two root
bytes, sixteen pattern-table bytes and thirteen track bytes through the first
note. Every replacement selection has 64 reads: six root-list bytes, 32
pattern-table bytes and 26 track bytes. The authored source accounts for the
replacement's whole-score validation and live playback passes; validation
also visits the root terminator before the live pass starts.

The principal residual delay is before root traversal, not in track decoding:

| Program/model | Last directory read to first root read | Last track read to expected pitch setup |
| --- | ---: | ---: |
| Original SGB1 | 25.466–48.004 ms | 0.517–0.538 ms |
| Original SGB2 | 25.516–28.007 ms | 0.517–0.538 ms |
| Replacement, either model | 0.131 ms | 1.543–1.555 ms |

These use the existing 2048-half-clock/ms scale. In every original capture,
the gap from the last root read to the first pattern-table read is 0.033 ms,
and the gap from the last pattern-table read to the first track read is
0.679 ms. The original reaches expected pitch setup about 2.5 ms after its
first root read. The large SGB1 first-selection delay remains distinct from
later selections. This does not yet establish a polling period or timer law.

For example, the SGB2 active baseline's first selection reads its directory
word at 28.295/28.302 ms, starts root traversal at 55.896 ms, starts track
consumption at 56.887 ms, reads the first note at 57.830 ms, and sets expected
pitch at 58.368 ms. Adding both effect-STOP fields moves the directory-to-root
interval by only 0.006 ms. Most latency therefore precedes actual root/pattern
consumption on this workload. No private instructions were inspected to derive
that observation.

Next, sweep SOUND request spacing in single-frame increments while retaining
the distinct-note bank and uploaded assets. Measure directory-to-root waits
separately for first, active and completed selections on both models; retain
STOP controls and reserve held-out offsets. Establish how that interval changes
with request phase before implementing any dispatch pacing. Rapid reselection,
STOP/upload ownership and pinned-title validation remain required for a policy.

## Validation evidence

The public report/playback suite passes in 230.779 seconds, including sixteen
observed timelines and sixteen identical ordinary controls. The added authored
CLI contract checks all three report modes without original images. Eight
focused native/report CTests pass in 5.30 seconds; four shard-runner tests pass.
Discovery registers the traversal matrix exactly once among 92 public matrices
with its timeline-probe target. Python compilation and whitespace checks pass.
All shared metadata in the 32 private captures exactly matches the preceding
directory/handoff reports.

The unchanged deterministic DB image is
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Original identities remain SGB1
`a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376`
and SGB2
`c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6`.
Broad earlier firmware matrices, pinned-title playback, Windows execution,
independent emulators and hardware were not rerun. Original program
instructions, proprietary score data and assets were not implementation inputs.
